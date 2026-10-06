// Display-locked pacing logic (pure, no clocks of its own; see docs/FRAME_PACING.md):
//  * cadence        - which display refreshes start a new emulated frame
//  * input deadline - how long before the commit deadline input is sampled (just in time)
//  * present path   - whether the layer goes direct to the display (from update timestamps)
//  * backlog drain  - when a steady one-drawable backlog is drained
//  * build ahead    - whether GPU-heavy pictures (CRT model) are built one refresh ahead
//  * audio rate     - dynamic rate control resampling ratio (emulation at the display rate)
//  * resampler      - streaming Hermite resampler for the live audio path
//  * frame pacing / callback regularity counters (latency overlay, snapshot, perf-smoke)
// Arithmetic mirrors the original Swift implementation operation by operation (the library is
// built with -ffp-contract=off), so decisions are bit-identical.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <vector>

#include "common.hpp"

using rnf::fail;

namespace {

constexpr double kMinRefresh = 1.0 / 500;
constexpr double kMaxRefresh = 1.0 / 24;
constexpr int kDeltaWindow = 15;

// Fixed-capacity ring that appends until full, then overwrites the oldest entry.
template <typename T>
struct Ring {
  explicit Ring(size_t cap = 0) : cap(cap) {}
  size_t cap;
  std::vector<T> v;
  size_t head = 0;
  void add(T x) {
    if (v.size() < cap) v.push_back(x);
    else { v[head] = x; head = (head + 1) % cap; }
  }
  void clear() { v.clear(); head = 0; }
};

int refreshesPerFrame(double refresh, double framePeriod) {
  if (!(refresh > 0)) return 0;
  double k = std::round(framePeriod / refresh);
  if (!(k >= 1) || !(std::fabs(k * refresh - framePeriod) / framePeriod <= RNF_CADENCE_LOCK_TOLERANCE)) return 0;
  return int(k);
}

}  // namespace

struct rnf_cadence {
  double framePeriod = rnf_frame_period();
  double refresh = 0;
  bool hasLastPresentation = false;
  double lastPresentation = 0;
  bool hasLastFrameTime = false;
  double lastFrameTime = 0;
  double contentTime = 0;
  Ring<double> deltas{kDeltaWindow};
};

struct rnf_input_deadline {
  static constexpr double binWidth = 0.0001;
  static constexpr int binCount = 101;
  static constexpr double quantile = 0.995;
  std::vector<uint8_t> ring = std::vector<uint8_t>(RNF_INPUT_DEADLINE_WINDOW, 0);
  std::vector<int> bins = std::vector<int>(binCount, 0);
  int filled = 0, head = 0;
  double penalty = 0;
  int clean = 0;
  uint64_t misses = 0;

  double workQuantile() const {
    if (filled <= 0) return 0;
    int need = int(std::ceil(double(filled) * quantile));
    int sum = 0;
    for (int i = 0; i < binCount; ++i) {
      sum += bins[size_t(i)];
      if (sum >= need) return double(i + 1) * binWidth;
    }
    return double(binCount) * binWidth;
  }
};

struct rnf_present_path {
  Ring<double> delays{RNF_PRESENT_PATH_WINDOW};
  double maxDelay() const {
    if (delays.v.empty()) return 0;
    return *std::max_element(delays.v.begin(), delays.v.end());
  }
};

struct rnf_build_ahead {
  bool active = false;
  Ring<double> times{RNF_BUILD_AHEAD_WINDOW};
  Ring<double> delays{RNF_BUILD_AHEAD_DELAY_WINDOW};
  bool p90(double& out) const {
    if (times.v.size() < RNF_BUILD_AHEAD_WINDOW / 4) return false;
    std::vector<double> s = times.v;
    std::sort(s.begin(), s.end());
    out = s[size_t(std::round(double(s.size() - 1) * 0.9))];
    return true;
  }
};

struct rnf_audio_rate {
  double targetFill = 0;
  double base = 1, ratio = 1;
  bool hasSmoothed = false;
  double smoothed = 0;
};

struct rnf_resampler {
  float history[3] = {0, 0, 0};
  double position = 0;
  bool primed = false;
  std::vector<float> work;
};

struct rnf_frame_pacing {
  uint64_t presents = 0, hitches = 0, skipped = 0;
  double windowMax = 0;
  bool hasLast = false;
  uint64_t lastFrame = 0;
  double lastTime = 0;
};

struct rnf_callback_regularity {
  double lateAfter = 0, idleAfter = 0.25;
  uint64_t count = 0, late = 0;
  double windowMaxGap = 0;
  bool hasLast = false;
  double lastTime = 0;
};

template <typename T>
static T* cloneOf(const T* p) {
  if (!p) return nullptr;
  try { return new T(*p); } catch (...) { return nullptr; }
}

extern "C" {

// ------------------------------------------------------------------ cadence
rnf_cadence* rnf_cadence_new(double frame_period) {
  try {
    auto* c = new rnf_cadence;
    if (frame_period > 0) c->framePeriod = frame_period;
    return c;
  } catch (...) { return nullptr; }
}
rnf_cadence* rnf_cadence_clone(const rnf_cadence* c) { return cloneOf(c); }
void rnf_cadence_free(rnf_cadence* c) { delete c; }

int rnf_refreshes_per_frame(double refresh, double frame_period) {
  return refreshesPerFrame(refresh, frame_period > 0 ? frame_period : rnf_frame_period());
}

int rnf_cadence_refresh(rnf_cadence* c, double t) {
  if (!c) return 0;
  if (c->hasLastPresentation && t > c->lastPresentation) {
    double d = t - c->lastPresentation;
    if (d >= kMinRefresh && d <= kMaxRefresh) {
      c->deltas.add(d);
      std::vector<double> s = c->deltas.v;
      std::sort(s.begin(), s.end());
      double median = s[s.size() / 2];
      if (c->refresh == 0 || std::fabs(c->refresh - median) > 0.1 * median) {
        // First estimate, another display, or a wrong estimate from odd timestamps.
        c->refresh = median;
      } else if (std::fabs(d - c->refresh) < 0.25 * c->refresh) {
        c->refresh += (d - c->refresh) * 0.05;  // refine (skipped callbacks are ignored)
      }
    }
    // Other deltas are not one refresh apart (duplicate / bogus timestamps, or a gap).
  }
  c->hasLastPresentation = true;
  c->lastPresentation = t;
  if (!(c->refresh > 0) || !c->hasLastFrameTime) {
    c->hasLastFrameTime = true;
    c->lastFrameTime = t;
    c->contentTime = t + c->framePeriod;
    return 1;
  }
  double last = c->lastFrameTime;
  if (t <= last) return 0;
  int k = refreshesPerFrame(c->refresh, c->framePeriod);
  if (k) {
    // Every k-th refresh after the previous frame (half a refresh of tolerance absorbs timestamp
    // noise; a refresh without a callback starts the frame at the next callback).
    if (!(t - last >= (double(k) - 0.5) * c->refresh)) return 0;
    c->lastFrameTime = t;
    c->contentTime = t + c->framePeriod;
    return 1;
  }
  // Free: the refresh nearest to the ideal frame time; resynchronise after a long gap.
  if (!(t >= c->contentTime - c->refresh / 2)) return 0;
  c->contentTime = t - c->contentTime > 2 * c->framePeriod ? t + c->framePeriod : c->contentTime + c->framePeriod;
  c->lastFrameTime = t;
  return 1;
}

double rnf_cadence_refresh_interval(const rnf_cadence* c) { return c ? c->refresh : 0; }
int rnf_cadence_refreshes_per_frame(const rnf_cadence* c) { return c ? refreshesPerFrame(c->refresh, c->framePeriod) : 0; }
double rnf_cadence_emulation_rate(const rnf_cadence* c) {
  if (!c) return 0;
  int k = refreshesPerFrame(c->refresh, c->framePeriod);
  if (!k) return 1 / c->framePeriod;
  return 1 / (double(k) * c->refresh);
}
double rnf_cadence_expected_frame_interval(const rnf_cadence* c) {
  if (!c) return 0;
  int k = refreshesPerFrame(c->refresh, c->framePeriod);
  if (!k) return c->framePeriod;
  return double(k) * c->refresh;
}
double rnf_cadence_frame_period(const rnf_cadence* c) { return c ? c->framePeriod : 0; }

// ------------------------------------------------------------------ input deadline
rnf_input_deadline* rnf_input_deadline_new(void) {
  try { return new rnf_input_deadline; } catch (...) { return nullptr; }
}
rnf_input_deadline* rnf_input_deadline_clone(const rnf_input_deadline* d) { return cloneOf(d); }
void rnf_input_deadline_free(rnf_input_deadline* d) { delete d; }

void rnf_input_deadline_observe_work(rnf_input_deadline* d, double seconds) {
  if (!d) return;
  double v = seconds / rnf_input_deadline::binWidth;
  int bin = 0;
  if (v >= double(rnf_input_deadline::binCount - 1)) bin = rnf_input_deadline::binCount - 1;
  else if (v > 0) bin = int(v);  // truncation, like Int(_:)
  if (d->filled == RNF_INPUT_DEADLINE_WINDOW) d->bins[d->ring[size_t(d->head)]] -= 1;
  else d->filled += 1;
  d->ring[size_t(d->head)] = uint8_t(bin);
  d->bins[size_t(bin)] += 1;
  d->head = (d->head + 1) % RNF_INPUT_DEADLINE_WINDOW;
  d->clean += 1;
  if (d->clean >= RNF_INPUT_DEADLINE_DECAY_AFTER) {
    d->clean = 0;
    d->penalty = std::max(0.0, d->penalty - RNF_INPUT_DEADLINE_PENALTY_DECAY);
  }
}

void rnf_input_deadline_observe_miss(rnf_input_deadline* d) {
  if (!d) return;
  d->misses += 1;
  d->clean = 0;
  d->penalty = std::min(RNF_INPUT_DEADLINE_MAX_PENALTY, d->penalty + RNF_INPUT_DEADLINE_MISS_PENALTY);
}

double rnf_input_deadline_lead(const rnf_input_deadline* d) {
  if (!d) return RNF_INPUT_DEADLINE_MIN_LEAD;
  return std::min(RNF_INPUT_DEADLINE_MAX_LEAD,
                  std::max(RNF_INPUT_DEADLINE_MIN_LEAD, d->workQuantile() + RNF_INPUT_DEADLINE_MARGIN + d->penalty));
}
double rnf_input_deadline_work_quantile(const rnf_input_deadline* d) { return d ? d->workQuantile() : 0; }
double rnf_input_deadline_penalty(const rnf_input_deadline* d) { return d ? d->penalty : 0; }
uint64_t rnf_input_deadline_misses(const rnf_input_deadline* d) { return d ? d->misses : 0; }

// ------------------------------------------------------------------ present path / backlog
rnf_present_path* rnf_present_path_new(void) {
  try { return new rnf_present_path; } catch (...) { return nullptr; }
}
rnf_present_path* rnf_present_path_clone(const rnf_present_path* p) { return cloneOf(p); }
void rnf_present_path_free(rnf_present_path* p) { delete p; }
void rnf_present_path_observe(rnf_present_path* p, double present_delay) {
  if (!p || !(present_delay > 0)) return;
  p->delays.add(present_delay);
}
void rnf_present_path_reset(rnf_present_path* p) { if (p) p->delays.clear(); }
double rnf_present_path_max_delay(const rnf_present_path* p) { return p ? p->maxDelay() : 0; }
int rnf_present_path_is_direct(const rnf_present_path* p, double refresh) {
  if (!p) return 0;
  return p->delays.v.size() >= RNF_PRESENT_PATH_WINDOW / 4 && refresh > 0 && p->maxDelay() < 1.5 * refresh;
}

rnf_backlog_policy rnf_backlog_drain_policy(int variable_refresh, int direct) {
  if (!variable_refresh && direct) return {RNF_BACKLOG_FAST_LATE_RUN, RNF_BACKLOG_FAST_MIN_INTERVAL};
  return {RNF_BACKLOG_RARE_LATE_RUN, RNF_BACKLOG_RARE_MIN_INTERVAL};
}

// ------------------------------------------------------------------ build ahead
rnf_build_ahead* rnf_build_ahead_new(void) {
  try { return new rnf_build_ahead; } catch (...) { return nullptr; }
}
rnf_build_ahead* rnf_build_ahead_clone(const rnf_build_ahead* b) { return cloneOf(b); }
void rnf_build_ahead_free(rnf_build_ahead* b) { delete b; }
void rnf_build_ahead_add(rnf_build_ahead* b, double s) { if (b) b->times.add(s); }
void rnf_build_ahead_reset(rnf_build_ahead* b) {
  if (!b) return;
  b->times.clear();
  b->active = false;
}
void rnf_build_ahead_update(rnf_build_ahead* b, double present_delay) {
  if (!b || !(present_delay > 0)) return;
  b->delays.add(present_delay);
  double q;
  if (!b->p90(q) || b->delays.v.empty()) return;
  double budget = *std::max_element(b->delays.v.begin(), b->delays.v.end());
  if (!b->active && q > budget - RNF_BUILD_AHEAD_ENTER_MARGIN) b->active = true;
  else if (b->active && q < budget - RNF_BUILD_AHEAD_LEAVE_MARGIN) b->active = false;
}
int rnf_build_ahead_active(const rnf_build_ahead* b) { return b && b->active; }
int rnf_build_ahead_p90(const rnf_build_ahead* b, double* out) {
  double q;
  if (!b || !b->p90(q)) return 0;
  if (out) *out = q;
  return 1;
}

// ------------------------------------------------------------------ audio rate control
rnf_audio_rate* rnf_audio_rate_new(double target_fill) {
  try {
    auto* a = new rnf_audio_rate;
    a->targetFill = target_fill;
    return a;
  } catch (...) { return nullptr; }
}
rnf_audio_rate* rnf_audio_rate_clone(const rnf_audio_rate* a) { return cloneOf(a); }
void rnf_audio_rate_free(rnf_audio_rate* a) { delete a; }
void rnf_audio_rate_set_frame_rate(rnf_audio_rate* a, double rate, double nominal) {
  if (!a || !(rate > 0)) return;
  if (!(nominal > 0)) nominal = 1 / rnf_frame_period();
  a->base = nominal / rate;
}
void rnf_audio_rate_reset(rnf_audio_rate* a) {
  if (!a) return;
  a->hasSmoothed = false;
  a->ratio = a->base;
}
double rnf_audio_rate_update(rnf_audio_rate* a, double fill) {
  if (!a) return 1;
  double f = a->hasSmoothed ? a->smoothed + (fill - a->smoothed) * RNF_DRC_FILL_SMOOTHING : fill;
  a->hasSmoothed = true;
  a->smoothed = f;
  double error = (a->targetFill - f) / a->targetFill;
  double adj = std::max(-RNF_DRC_MAX_DEVIATION, std::min(RNF_DRC_MAX_DEVIATION, error * RNF_DRC_GAIN));
  a->ratio = a->base * (1 + adj);
  return a->ratio;
}
double rnf_audio_rate_base(const rnf_audio_rate* a) { return a ? a->base : 1; }
double rnf_audio_rate_ratio(const rnf_audio_rate* a) { return a ? a->ratio : 1; }
double rnf_audio_rate_target_fill(const rnf_audio_rate* a) { return a ? a->targetFill : 0; }
int rnf_audio_rate_smoothed_fill(const rnf_audio_rate* a, double* out) {
  if (!a || !a->hasSmoothed) return 0;
  if (out) *out = a->smoothed;
  return 1;
}

// ------------------------------------------------------------------ resampler
rnf_resampler* rnf_resampler_new(void) {
  try { return new rnf_resampler; } catch (...) { return nullptr; }
}
rnf_resampler* rnf_resampler_clone(const rnf_resampler* r) { return cloneOf(r); }
void rnf_resampler_free(rnf_resampler* r) { delete r; }
void rnf_resampler_reset(rnf_resampler* r) {
  if (!r) return;
  r->history[0] = r->history[1] = r->history[2] = 0;
  r->position = 0;
  r->primed = false;
}
size_t rnf_resampler_output_bound(size_t n) { return 2 * n + 8; }

rn_status rnf_resampler_process(rnf_resampler* r, const int16_t* in, size_t n, double ratio, int16_t* out,
                                size_t out_cap, size_t* out_count) {
  if (out_count) *out_count = 0;
  if (!r) return fail(RN_ERR_INVALID_ARG, "null resampler");
  if (n == 0) return RN_OK;
  if (!in || !out || out_cap < rnf_resampler_output_bound(n)) return fail(RN_ERR_INVALID_ARG, "output buffer too small");
  RNF_GUARD_BEGIN
  if (!r->primed) {
    // Start on the first sample (no ramp from silence).
    float first = float(in[0]);
    r->history[0] = r->history[1] = r->history[2] = first;
    r->primed = true;
  }
  // work = x[-3], x[-2], x[-1], x[0] ... x[n-1]
  std::vector<float>& work = r->work;
  work.clear();
  work.insert(work.end(), r->history, r->history + 3);
  for (size_t i = 0; i < n; ++i) work.push_back(float(in[i]));
  double step = 1 / std::max(0.5, std::min(2.0, ratio));
  long long wn = (long long)work.size();
  double p = r->position;
  size_t written = 0;
  for (;;) {
    double fl = std::floor(p);
    long long i = (long long)fl + 2;
    if (i + 2 >= wn) break;
    float t = float(p - fl);
    float y0 = work[size_t(i - 1)], y1 = work[size_t(i)], y2 = work[size_t(i + 1)], y3 = work[size_t(i + 2)];
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    float v = ((c3 * t + c2) * t + c1) * t + y1;
    float rv = std::round(v);
    out[written++] = int16_t(std::max(-32768.0f, std::min(32767.0f, rv)));
    p += step;
  }
  // Keep the last three samples; positions are re-based on the new x[-1].
  r->position = p - double(n);
  r->history[0] = work[size_t(wn - 3)];
  r->history[1] = work[size_t(wn - 2)];
  r->history[2] = work[size_t(wn - 1)];
  if (out_count) *out_count = written;
  return RN_OK;
  RNF_GUARD_END(RN_ERR_INTERNAL)
}

// ------------------------------------------------------------------ frame pacing counters
rnf_frame_pacing* rnf_frame_pacing_new(void) {
  try { return new rnf_frame_pacing; } catch (...) { return nullptr; }
}
rnf_frame_pacing* rnf_frame_pacing_clone(const rnf_frame_pacing* p) { return cloneOf(p); }
void rnf_frame_pacing_free(rnf_frame_pacing* p) { delete p; }
double rnf_frame_pacing_hitch_interval(void) { return rnf_frame_period() * 1.5; }

void rnf_frame_pacing_present(rnf_frame_pacing* p, uint64_t frame, double time) {
  if (!p) return;
  p->presents += 1;
  bool continuous = p->hasLast && frame > p->lastFrame && frame - p->lastFrame <= 8 && time > p->lastTime &&
                    time - p->lastTime < RNF_FRAME_PACING_CONTINUITY_LIMIT;
  if (continuous) {
    double dt = time - p->lastTime;
    p->skipped += frame - p->lastFrame - 1;
    if (frame - p->lastFrame == 1 && dt > rnf_frame_pacing_hitch_interval()) p->hitches += 1;
    p->windowMax = std::max(p->windowMax, dt);
  }
  p->hasLast = true;
  p->lastFrame = frame;
  p->lastTime = time;
}
uint64_t rnf_frame_pacing_presents(const rnf_frame_pacing* p) { return p ? p->presents : 0; }
uint64_t rnf_frame_pacing_hitches(const rnf_frame_pacing* p) { return p ? p->hitches : 0; }
uint64_t rnf_frame_pacing_skipped(const rnf_frame_pacing* p) { return p ? p->skipped : 0; }
double rnf_frame_pacing_window_max(const rnf_frame_pacing* p) { return p ? p->windowMax : 0; }
double rnf_frame_pacing_take_window_max(rnf_frame_pacing* p) {
  if (!p) return 0;
  double v = p->windowMax;
  p->windowMax = 0;
  return v;
}
int rnf_frame_pacing_last_present(const rnf_frame_pacing* p, uint64_t* frame, double* time) {
  if (!p || !p->hasLast) return 0;
  if (frame) *frame = p->lastFrame;
  if (time) *time = p->lastTime;
  return 1;
}

// ------------------------------------------------------------------ callback regularity
rnf_callback_regularity* rnf_callback_regularity_new(double late_after, double idle_after) {
  try {
    auto* r = new rnf_callback_regularity;
    r->lateAfter = late_after;
    r->idleAfter = idle_after;
    return r;
  } catch (...) { return nullptr; }
}
rnf_callback_regularity* rnf_callback_regularity_clone(const rnf_callback_regularity* r) { return cloneOf(r); }
void rnf_callback_regularity_free(rnf_callback_regularity* r) { delete r; }
void rnf_callback_regularity_tick(rnf_callback_regularity* r, double t) {
  if (!r) return;
  r->count += 1;
  if (r->hasLast && t > r->lastTime && t - r->lastTime < r->idleAfter) {
    double gap = t - r->lastTime;
    if (gap > r->lateAfter) r->late += 1;
    r->windowMaxGap = std::max(r->windowMaxGap, gap);
  }
  r->hasLast = true;
  r->lastTime = t;
}
void rnf_callback_regularity_set_late_after(rnf_callback_regularity* r, double v) { if (r) r->lateAfter = v; }
double rnf_callback_regularity_late_after(const rnf_callback_regularity* r) { return r ? r->lateAfter : 0; }
double rnf_callback_regularity_idle_after(const rnf_callback_regularity* r) { return r ? r->idleAfter : 0; }
uint64_t rnf_callback_regularity_count(const rnf_callback_regularity* r) { return r ? r->count : 0; }
uint64_t rnf_callback_regularity_late(const rnf_callback_regularity* r) { return r ? r->late : 0; }
double rnf_callback_regularity_window_max(const rnf_callback_regularity* r) { return r ? r->windowMaxGap : 0; }
double rnf_callback_regularity_take_window_max(rnf_callback_regularity* r) {
  if (!r) return 0;
  double v = r->windowMaxGap;
  r->windowMaxGap = 0;
  return v;
}

}  // extern "C"
