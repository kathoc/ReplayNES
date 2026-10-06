// UI-free transport logic: record/replay toggle, slow toggle, paused D-pad stepping, practice A/B
// loop, rewind-animation frame history, the practice audio fade-out tail and fast-forward over
// the recorded take.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <vector>

#include "common.hpp"

using namespace rnf;

struct rnf_step_repeater {
  int direction = 0;
  int heldTicks = 0;
};

struct rnf_practice_loop {
  rnf_practice_phase phase = RNF_PHASE_PLAYING;
  double since = 0;
  int loops = 0;
};

struct rnf_frame_history {
  static constexpr size_t pixels = size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT;
  int capacity = 1;
  std::vector<uint32_t> storage;
  int head = 0;  // next write slot
  int count = 0;
};

struct rnf_fast_forward {
  bool active = false;
  bool restoreRecord = false;
};

template <typename T>
static T* cloneOf(const T* p) {
  if (!p) return nullptr;
  try { return new T(*p); } catch (...) { return nullptr; }
}

extern "C" {

// ------------------------------------------------------------------ slow / record toggle
int rnf_slow_toggled(int rate) { return rate == 1 ? 2 : 1; }
char* rnf_slow_label(int rate) {
  if (rate == 1) return dup(tr(RNF_L("Normal")));
  return dup(rate == 2 ? "1/2" : "1/4");
}

rnf_record_toggle_plan rnf_record_toggle(int recording, uint64_t frame, uint64_t take_length) {
  if (recording) {
    if (take_length == 0) return {0, 0, 0, 0};  // nothing recorded yet
    if (frame >= take_length) return {0, 1, 0, 1};
    return {0, 0, 0, 1};
  }
  return {1, 0, 0, 0};
}

int rnf_record_toggle_restarts_on_play(int recording, int practicing, uint64_t frame, uint64_t take_length) {
  return !recording && !practicing && take_length > 0 && frame >= take_length;
}

// ------------------------------------------------------------------ step repeater
rnf_step_repeater* rnf_step_repeater_new(void) {
  try { return new rnf_step_repeater; } catch (...) { return nullptr; }
}
rnf_step_repeater* rnf_step_repeater_clone(const rnf_step_repeater* r) { return cloneOf(r); }
void rnf_step_repeater_free(rnf_step_repeater* r) { delete r; }
int rnf_step_repeater_press(rnf_step_repeater* r, int dir) {
  if (!r) return 0;
  r->direction = dir;
  r->heldTicks = 0;
  return dir;
}
void rnf_step_repeater_release(rnf_step_repeater* r, int dir) {
  if (r && dir == r->direction) { r->direction = 0; r->heldTicks = 0; }
}
int rnf_step_repeater_tick(rnf_step_repeater* r) {
  if (!r || r->direction == 0) return 0;
  r->heldTicks += 1;
  if (r->heldTicks >= RNF_STEP_REPEAT_INITIAL_DELAY &&
      (r->heldTicks - RNF_STEP_REPEAT_INITIAL_DELAY) % RNF_STEP_REPEAT_INTERVAL == 0)
    return r->direction;
  return 0;
}
void rnf_step_repeater_reset(rnf_step_repeater* r) {
  if (r) { r->direction = 0; r->heldTicks = 0; }
}
int rnf_step_repeater_direction(const rnf_step_repeater* r) { return r ? r->direction : 0; }

// ------------------------------------------------------------------ practice loop
rnf_practice_loop* rnf_practice_loop_new(void) {
  try { return new rnf_practice_loop; } catch (...) { return nullptr; }
}
rnf_practice_loop* rnf_practice_loop_clone(const rnf_practice_loop* l) { return cloneOf(l); }
void rnf_practice_loop_free(rnf_practice_loop* l) { delete l; }

rnf_practice_action rnf_practice_loop_tick(rnf_practice_loop* l, double now, uint64_t counter, int has_length,
                                           uint64_t length) {
  if (!l) return {RNF_PRACTICE_STEP, 0};
  switch (l->phase) {
    case RNF_PHASE_PLAYING:
      if (has_length && length > 0 && counter >= length) {
        l->phase = RNF_PHASE_HOLDING;
        l->since = now;
        return {RNF_PRACTICE_BEGIN_HOLD, 0};
      }
      return {RNF_PRACTICE_STEP, 0};
    case RNF_PHASE_HOLDING:
      if (now - l->since >= RNF_PRACTICE_HOLD_SECONDS) {
        l->phase = RNF_PHASE_REWINDING;
        l->since = now;
        return {RNF_PRACTICE_REWIND_FRAME, 0};
      }
      return {RNF_PRACTICE_HOLD, 0};
    case RNF_PHASE_REWINDING: {
      double p = (now - l->since) / RNF_PRACTICE_REWIND_SECONDS;
      if (p >= 1) {
        l->phase = RNF_PHASE_PLAYING;
        l->since = 0;
        l->loops += 1;
        return {RNF_PRACTICE_RESTART, 0};
      }
      return {RNF_PRACTICE_REWIND_FRAME, std::max(0.0, p)};
    }
  }
  return {RNF_PRACTICE_STEP, 0};
}

void rnf_practice_loop_interrupt(rnf_practice_loop* l) {
  if (l) { l->phase = RNF_PHASE_PLAYING; l->since = 0; }
}
void rnf_practice_loop_reset(rnf_practice_loop* l) {
  if (l) { l->phase = RNF_PHASE_PLAYING; l->since = 0; l->loops = 0; }
}
rnf_practice_phase rnf_practice_loop_phase(const rnf_practice_loop* l, double* since) {
  if (!l) return RNF_PHASE_PLAYING;
  if (since) *since = l->since;
  return l->phase;
}
int rnf_practice_loop_loops(const rnf_practice_loop* l) { return l ? l->loops : 0; }

int rnf_practice_history_index(double back, int count) {
  if (count <= 0) return 0;
  double v = std::floor(back * double(count));
  int i = v <= 0 ? 0 : v >= double(count - 1) ? count - 1 : int(v);
  return std::min(count - 1, std::max(0, i));
}

// ------------------------------------------------------------------ frame history
rnf_frame_history* rnf_frame_history_new(int capacity) {
  try {
    auto* h = new rnf_frame_history;
    h->capacity = std::max(1, capacity);
    return h;
  } catch (...) { return nullptr; }
}
void rnf_frame_history_free(rnf_frame_history* h) { delete h; }
void rnf_frame_history_append(rnf_frame_history* h, const uint32_t* frame) {
  if (!h || !frame) return;
  try {
    if (h->storage.empty()) h->storage.assign(size_t(h->capacity) * rnf_frame_history::pixels, 0);
  } catch (...) {
    return;
  }
  std::copy(frame, frame + rnf_frame_history::pixels, h->storage.begin() + ptrdiff_t(size_t(h->head) * rnf_frame_history::pixels));
  h->head = (h->head + 1) % h->capacity;
  h->count = std::min(h->count + 1, h->capacity);
}
const uint32_t* rnf_frame_history_frame(const rnf_frame_history* h, int back) {
  if (!h || back < 0 || back >= h->count) return nullptr;
  int slot = (h->head - 1 - back + h->capacity * 2) % h->capacity;
  return h->storage.data() + size_t(slot) * rnf_frame_history::pixels;
}
int rnf_frame_history_count(const rnf_frame_history* h) { return h ? h->count : 0; }
int rnf_frame_history_capacity(const rnf_frame_history* h) { return h ? h->capacity : 0; }
void rnf_frame_history_clear(rnf_frame_history* h) {
  if (h) { h->head = 0; h->count = 0; }
}
void rnf_frame_history_release(rnf_frame_history* h) {
  if (!h) return;
  rnf_frame_history_clear(h);
  std::vector<uint32_t>().swap(h->storage);
}

// ------------------------------------------------------------------ audio fade
size_t rnf_audio_fade_tail(const int16_t* last, size_t n, int repeats, int16_t* out, size_t cap) {
  if (!last || n == 0 || repeats <= 0) return 0;
  size_t total = n * size_t(repeats);
  if (!out) return total;
  for (size_t i = 0; i < total && i < cap; ++i) {
    double g = 1.0 - double(i + 1) / double(total);  // linear to exactly 0
    double v = std::round(double(last[i % n]) * g * g);
    out[i] = int16_t(std::max(-32768.0, std::min(32767.0, v)));
  }
  return total;
}

// ------------------------------------------------------------------ fast-forward
rnf_fast_forward* rnf_fast_forward_new(void) {
  try { return new rnf_fast_forward; } catch (...) { return nullptr; }
}
rnf_fast_forward* rnf_fast_forward_clone(const rnf_fast_forward* f) { return cloneOf(f); }
void rnf_fast_forward_free(rnf_fast_forward* f) { delete f; }

rn_status rnf_fast_forward_begin(rnf_fast_forward* f, rn_session* s) {
  if (!f || !s) return fail(RN_ERR_INVALID_ARG, "null argument");
  if (f->active) return RN_OK;
  f->active = true;
  f->restoreRecord = rn_get_mode(s) == RN_MODE_RECORD;
  if (f->restoreRecord) return rn_set_mode(s, RN_MODE_REPLAY);
  return RN_OK;
}

rn_status rnf_fast_forward_step(const rnf_fast_forward* f, rn_session* s, int n, int* done, int* reached_end) {
  if (done) *done = 0;
  if (reached_end) *reached_end = 0;
  if (!f || !s) return fail(RN_ERR_INVALID_ARG, "null argument");
  int d = 0;
  for (int i = 0; i < n; ++i) {
    if (rn_frame(s) >= rn_take_length(s)) {
      if (done) *done = d;
      if (reached_end) *reached_end = 1;
      return RN_OK;
    }
    uint64_t before = rn_frame(s);
    rn_step_info info{};
    rn_status st = rn_step(s, 0, 0, 0, &info);
    if (st != RN_OK) {
      if (done) *done = d;
      return st;
    }
    if (info.end_of_take != 0 && info.frame == before) {
      if (done) *done = d;
      if (reached_end) *reached_end = 1;
      return RN_OK;
    }
    d += 1;
  }
  if (done) *done = d;
  if (reached_end) *reached_end = rn_frame(s) >= rn_take_length(s);
  return RN_OK;
}

rn_status rnf_fast_forward_end(rnf_fast_forward* f, rn_session* s) {
  if (!f || !s) return fail(RN_ERR_INVALID_ARG, "null argument");
  if (!f->active) return RN_OK;
  f->active = false;
  if (f->restoreRecord) {
    f->restoreRecord = false;
    return rn_set_mode(s, RN_MODE_RECORD);
  }
  return RN_OK;
}

int rnf_fast_forward_active(const rnf_fast_forward* f) { return f && f->active; }
int rnf_fast_forward_shows_recording(const rnf_fast_forward* f, rn_session* s) {
  if (!f || !s) return 0;
  return rn_get_mode(s) == RN_MODE_RECORD || (f->active && f->restoreRecord);
}

}  // extern "C"
