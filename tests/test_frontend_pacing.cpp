// Frontend core: display-locked pacing (cadence, input deadline, present path, backlog drain,
// build ahead, audio rate control), the DRC resampler and the frame pacing counters.
// Ported from the macOS DisplayPacingTests / FramePacingTests.
#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {
const double P = rnf_frame_period();
const double kPi = 3.14159265358979323846;

std::vector<int> run(rnf_cadence* c, double refresh, int count, double start = 1000, std::set<int> drop = {}) {
  std::vector<int> frames;
  for (int i = 0; i < count; ++i) {
    if (drop.count(i)) continue;
    if (rnf_cadence_refresh(c, start + double(i) * refresh)) frames.push_back(i);
  }
  return frames;
}

struct Cadence {
  rnf_cadence* c = rnf_cadence_new(0);
  ~Cadence() { rnf_cadence_free(c); }
};
}  // namespace

TEST_CASE("locked refresh multiples") {
  CHECK_EQ(rnf_refreshes_per_frame(1.0 / 60, 0), 1);
  CHECK_EQ(rnf_refreshes_per_frame(1.0 / 120, 0), 2);
  CHECK_EQ(rnf_refreshes_per_frame(1.0 / 240, 0), 4);
  CHECK_EQ(rnf_refreshes_per_frame(1.0 / 59.94, 0), 1);
  CHECK_EQ(rnf_refreshes_per_frame(1.0 / 144, 0), 0);
  CHECK_EQ(rnf_refreshes_per_frame(1.0 / 75, 0), 0);
  CHECK_EQ(rnf_refreshes_per_frame(1.0 / 50, 0), 0);
}

TEST_CASE("every 120 Hz refresh pair is one frame") {
  Cadence c;
  auto f = run(c.c, 1.0 / 120, 1200);
  CHECK_EQ(f.size(), size_t(600));
  for (size_t i = 1; i < f.size(); ++i) CHECK_EQ(f[i] - f[i - 1], 2);
  CHECK(near(rnf_cadence_emulation_rate(c.c), 60, 1e-6));
  CHECK(near(rnf_cadence_expected_frame_interval(c.c), 2.0 / 120, 1e-9));
}

TEST_CASE("every 60 Hz refresh is one frame") {
  Cadence c;
  auto f = run(c.c, 1.0 / 60, 600);
  REQUIRE_EQ(f.size(), size_t(600));
  for (int i = 0; i < 600; ++i) CHECK_EQ(f[size_t(i)], i);
}

TEST_CASE("missed callback does not lose a frame") {
  Cadence c;
  auto f = run(c.c, 1.0 / 120, 200, 1000, {102});
  auto has = [&](int v) { return std::find(f.begin(), f.end(), v) != f.end(); };
  CHECK(has(100));
  CHECK(has(103));
  CHECK_FALSE(has(104));
  CHECK(has(105));
  CHECK_EQ(f.size(), size_t(100));
}

TEST_CASE("timestamp noise keeps the cadence") {
  Cadence c;
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> jitter(-200, 200);
  int frames = 0;
  for (int i = 0; i < 2400; ++i)
    if (rnf_cadence_refresh(c.c, 50 + double(i) / 120 + double(jitter(rng)) * 1e-6)) ++frames;
  CHECK_EQ(frames, 1200);
}

TEST_CASE("bogus timestamps do not break the estimate") {
  Cadence c;
  int frames = 0;
  for (int i = 0; i < 1200; ++i) {
    double t = 10 + double(i) / 120;
    if (rnf_cadence_refresh(c.c, t)) ++frames;
    if (i % 50 == 7 && rnf_cadence_refresh(c.c, t + 1e-6)) ++frames;
  }
  CHECK(near(rnf_cadence_refresh_interval(c.c), 1.0 / 120, 1e-6));
  CHECK_EQ(rnf_cadence_refreshes_per_frame(c.c), 2);
  CHECK_EQ(frames, 600);
}

TEST_CASE("odd delta does not stick the estimate") {
  Cadence c;
  int frames = 0;
  for (int i = 0; i < 1200; ++i) {
    double t = 10 + double(i) / 120;
    if (rnf_cadence_refresh(c.c, t)) ++frames;
    if (i == 100 && rnf_cadence_refresh(c.c, t + 0.0035)) ++frames;
  }
  CHECK(near(rnf_cadence_refresh_interval(c.c), 1.0 / 120, 1e-5));
  CHECK_EQ(rnf_cadence_refreshes_per_frame(c.c), 2);
  CHECK(std::abs(frames - 600) <= 2);
}

TEST_CASE("display change is followed") {
  Cadence c;
  for (int i = 0; i < 240; ++i) rnf_cadence_refresh(c.c, 10 + double(i) / 60);
  CHECK_EQ(rnf_cadence_refreshes_per_frame(c.c), 1);
  for (int i = 1; i <= 240; ++i) rnf_cadence_refresh(c.c, 14 + double(i) / 120);
  CHECK(near(rnf_cadence_refresh_interval(c.c), 1.0 / 120, 1e-5));
  CHECK_EQ(rnf_cadence_refreshes_per_frame(c.c), 2);
}

TEST_CASE("free cadence keeps the NTSC rate") {
  Cadence c;
  auto f = run(c.c, 1.0 / 144, 144 * 60);
  CHECK(near(double(f.size()), 60 / P, 2));
  CHECK_EQ(rnf_cadence_refreshes_per_frame(c.c), 0);
  CHECK(near(rnf_cadence_emulation_rate(c.c), 1 / P, 1e-9));
}

TEST_CASE("cadence clone is independent") {
  Cadence c;
  run(c.c, 1.0 / 120, 100);
  rnf_cadence* copy = rnf_cadence_clone(c.c);
  int a = rnf_cadence_refresh(c.c, 1000 + 100.0 / 120);
  int b = rnf_cadence_refresh(copy, 1000 + 100.0 / 120);
  CHECK_EQ(a, b);
  CHECK_EQ(rnf_cadence_refresh_interval(copy), rnf_cadence_refresh_interval(c.c));
  rnf_cadence_free(copy);
}

TEST_CASE("input deadline follows work and misses") {
  rnf_input_deadline* d = rnf_input_deadline_new();
  CHECK(near(rnf_input_deadline_lead(d), RNF_INPUT_DEADLINE_MIN_LEAD, 1e-12));
  for (int i = 0; i < 600; ++i) rnf_input_deadline_observe_work(d, 0.00105);
  CHECK(near(rnf_input_deadline_work_quantile(d), 0.0011, 1e-9));
  CHECK(near(rnf_input_deadline_lead(d), 0.0011 + RNF_INPUT_DEADLINE_MARGIN, 1e-9));
  rnf_input_deadline_observe_work(d, 0.050);  // a single stall does not move the lead
  CHECK(near(rnf_input_deadline_lead(d), 0.0011 + RNF_INPUT_DEADLINE_MARGIN, 1e-9));
  for (int i = 0; i < 5; ++i) rnf_input_deadline_observe_work(d, 0.00405);  // a run of heavier frames does
  CHECK(near(rnf_input_deadline_work_quantile(d), 0.0041, 1e-9));
  for (int i = 0; i < 600; ++i) rnf_input_deadline_observe_work(d, 0.00105);
  CHECK(near(rnf_input_deadline_work_quantile(d), 0.0011, 1e-9));
  rnf_input_deadline_observe_miss(d);
  CHECK(near(rnf_input_deadline_lead(d), 0.0011 + RNF_INPUT_DEADLINE_MARGIN + RNF_INPUT_DEADLINE_MISS_PENALTY, 1e-9));
  for (int i = 0; i < 20; ++i) rnf_input_deadline_observe_miss(d);
  CHECK(near(rnf_input_deadline_penalty(d), RNF_INPUT_DEADLINE_MAX_PENALTY, 1e-12));
  CHECK_EQ(rnf_input_deadline_misses(d), uint64_t(21));
  for (int i = 0; i < RNF_INPUT_DEADLINE_DECAY_AFTER * 30; ++i) rnf_input_deadline_observe_work(d, 0.00105);
  CHECK(near(rnf_input_deadline_penalty(d), 0, 1e-12));
  for (int i = 0; i < 600; ++i) rnf_input_deadline_observe_work(d, 0.1);
  CHECK_EQ(rnf_input_deadline_lead(d), RNF_INPUT_DEADLINE_MAX_LEAD);
  rnf_input_deadline_free(d);
}

TEST_CASE("input deadline limits are configurable") {
  rnf_input_deadline* d = rnf_input_deadline_new();
  CHECK_EQ(rnf_input_deadline_max_lead(d), RNF_INPUT_DEADLINE_MAX_LEAD);
  // Linux: the deadline is the vblank, so the lead may exceed one refresh (2 refreshes at 90 Hz).
  rnf_input_deadline_set_limits(d, 0, 0.0222, 0.0202);
  for (int i = 0; i < 600; ++i) rnf_input_deadline_observe_work(d, 0.00105);
  for (int i = 0; i < 30; ++i) rnf_input_deadline_observe_miss(d);
  CHECK(near(rnf_input_deadline_penalty(d), 30 * RNF_INPUT_DEADLINE_MISS_PENALTY, 1e-9));
  CHECK(near(rnf_input_deadline_lead(d), 0.0011 + RNF_INPUT_DEADLINE_MARGIN + 0.015, 1e-9));
  for (int i = 0; i < 20; ++i) rnf_input_deadline_observe_miss(d);
  CHECK(near(rnf_input_deadline_lead(d), 0.0222, 1e-12));
  // Back to the defaults: the penalty is clamped to the smaller maximum.
  rnf_input_deadline_set_limits(d, RNF_INPUT_DEADLINE_MIN_LEAD, RNF_INPUT_DEADLINE_MAX_LEAD, RNF_INPUT_DEADLINE_MAX_PENALTY);
  CHECK(near(rnf_input_deadline_penalty(d), RNF_INPUT_DEADLINE_MAX_PENALTY, 1e-12));
  CHECK(near(rnf_input_deadline_lead(d), 0.0011 + RNF_INPUT_DEADLINE_MARGIN + RNF_INPUT_DEADLINE_MAX_PENALTY, 1e-9));
  rnf_input_deadline_free(d);
}

TEST_CASE("audio rate control converges") {
  rnf_audio_rate* drc = rnf_audio_rate_new(1400);
  rnf_audio_rate_set_frame_rate(drc, 60, 0);
  CHECK(near(rnf_audio_rate_base(drc), (1 / P) / 60, 1e-12));
  double fill = 1400, minFill = fill, maxFill = fill;
  const double perFrame = 48000 * P;
  for (int frame = 0; frame < 60 * 600; ++frame) {
    double r = rnf_audio_rate_update(drc, fill);
    CHECK(std::fabs(r / rnf_audio_rate_base(drc) - 1) <= RNF_DRC_MAX_DEVIATION + 1e-12);
    fill += perFrame * r;
    fill -= 48000.0 / 60;
    if (frame > 600) { minFill = std::min(minFill, fill); maxFill = std::max(maxFill, fill); }
  }
  CHECK(near(minFill, 1400, 50));
  CHECK(near(maxFill, 1400, 50));
  double fill2 = 600;
  rnf_audio_rate* drc2 = rnf_audio_rate_new(1400);
  rnf_audio_rate_set_frame_rate(drc2, 60, 0);
  for (int i = 0; i < 60 * 600; ++i) {
    fill2 += perFrame * rnf_audio_rate_update(drc2, fill2);
    fill2 -= 48000.0 * 1.0003 / 60;
  }
  CHECK(near(fill2, 1400, 200));
  rnf_audio_rate_reset(drc2);
  double sm = 0;
  CHECK_EQ(rnf_audio_rate_smoothed_fill(drc2, &sm), 0);
  CHECK_EQ(rnf_audio_rate_ratio(drc2), rnf_audio_rate_base(drc2));
  rnf_audio_rate_free(drc);
  rnf_audio_rate_free(drc2);
}

TEST_CASE("resampler at unity is transparent") {
  rnf_resampler* r = rnf_resampler_new();
  std::vector<int16_t> input(2000), out;
  for (int i = 0; i < 2000; ++i) input[size_t(i)] = int16_t((i * 37) % 20000 - 10000);
  for (size_t chunk = 0; chunk < input.size(); chunk += 800) {
    size_t n = std::min<size_t>(800, input.size() - chunk);
    std::vector<int16_t> buf(rnf_resampler_output_bound(n));
    size_t got = 0;
    REQUIRE_EQ(rnf_resampler_process(r, input.data() + chunk, n, 1, buf.data(), buf.size(), &got), RN_OK);
    out.insert(out.end(), buf.begin(), buf.begin() + ptrdiff_t(got));
  }
  // One sample of delay (the first sample is repeated once; the last is held back as look-ahead).
  REQUIRE_EQ(out.size(), input.size() - 1);
  CHECK_EQ(out[0], input[0]);
  for (size_t i = 1; i < out.size(); ++i)
    if (out[i] != input[i - 1]) { FAIL("first difference at output sample " << i); }
  int16_t small[4];
  size_t got = 0;
  CHECK_EQ(rnf_resampler_process(r, input.data(), 800, 1, small, 4, &got), RN_ERR_INVALID_ARG);
  rnf_resampler_free(r);
}

TEST_CASE("resampler ratio and continuity") {
  rnf_resampler* r = rnf_resampler_new();
  const double ratio = 1.0016;
  double phase = 0;
  std::vector<int16_t> out;
  size_t inCount = 0;
  for (int k = 0; k < 600; ++k) {
    std::vector<int16_t> chunk(799);
    for (auto& s : chunk) { s = int16_t(20000 * std::sin(phase)); phase += 2 * kPi * 1000 / 48000; }
    inCount += chunk.size();
    std::vector<int16_t> buf(rnf_resampler_output_bound(chunk.size()));
    size_t got = 0;
    rnf_resampler_process(r, chunk.data(), chunk.size(), ratio, buf.data(), buf.size(), &got);
    out.insert(out.end(), buf.begin(), buf.begin() + ptrdiff_t(got));
  }
  CHECK(near(double(out.size()), double(inCount) * ratio, 3));
  const double maxStep = 20000 * 2 * kPi * 1000 / 48000 / ratio;
  for (size_t i = 1; i < out.size(); ++i) CHECK(std::fabs(double(out[i]) - double(out[i - 1])) <= maxStep * 1.02 + 2);
  rnf_resampler_free(r);
}

TEST_CASE("build ahead only for heavy pictures on the direct path") {
  const double direct = 1.0 / 120, composited = 2.0 / 120;
  rnf_build_ahead* b = rnf_build_ahead_new();
  for (int i = 0; i < 120; ++i) rnf_build_ahead_add(b, 0.0001);
  rnf_build_ahead_update(b, direct);
  CHECK_FALSE(rnf_build_ahead_active(b));
  rnf_build_ahead* crt = rnf_build_ahead_new();
  for (int i = 0; i < 29; ++i) {
    rnf_build_ahead_add(crt, 0.008);
    rnf_build_ahead_update(crt, direct);
    CHECK_FALSE(rnf_build_ahead_active(crt));
  }
  rnf_build_ahead_add(crt, 0.008);
  rnf_build_ahead_update(crt, direct);
  CHECK(rnf_build_ahead_active(crt));
  rnf_build_ahead* window = rnf_build_ahead_new();
  for (int i = 0; i < 120; ++i) rnf_build_ahead_add(window, 0.008);
  rnf_build_ahead_update(window, composited);
  CHECK_FALSE(rnf_build_ahead_active(window));
  rnf_build_ahead* mixed = rnf_build_ahead_new();
  for (int i = 0; i < 120; ++i) rnf_build_ahead_add(mixed, 0.008);
  rnf_build_ahead_update(mixed, composited);
  for (int i = 0; i < RNF_BUILD_AHEAD_DELAY_WINDOW - 1; ++i) rnf_build_ahead_update(mixed, direct);
  CHECK_FALSE(rnf_build_ahead_active(mixed));
  rnf_build_ahead_update(mixed, direct);
  CHECK(rnf_build_ahead_active(mixed));
  for (auto* p : {b, crt, window, mixed}) rnf_build_ahead_free(p);
}

TEST_CASE("build ahead hysteresis") {
  const double direct = 1.0 / 120;
  rnf_build_ahead* b = rnf_build_ahead_new();
  for (int i = 0; i < 120; ++i) rnf_build_ahead_add(b, 0.008);
  rnf_build_ahead_update(b, direct);
  CHECK(rnf_build_ahead_active(b));
  for (int i = 0; i < 120; ++i) rnf_build_ahead_add(b, 0.005);
  rnf_build_ahead_update(b, direct);
  CHECK(rnf_build_ahead_active(b));
  for (int i = 0; i < 120; ++i) rnf_build_ahead_add(b, 0.003);
  rnf_build_ahead_update(b, direct);
  CHECK_FALSE(rnf_build_ahead_active(b));
  for (int i = 0; i < 120; ++i) rnf_build_ahead_add(b, 0.008);
  rnf_build_ahead_update(b, direct);
  rnf_build_ahead_reset(b);
  CHECK_FALSE(rnf_build_ahead_active(b));
  double q;
  CHECK_EQ(rnf_build_ahead_p90(b, &q), 0);
  rnf_build_ahead_free(b);
}

TEST_CASE("backlog drain policy") {
  rnf_backlog_policy fast = rnf_backlog_drain_policy(0, 1);
  CHECK_EQ(fast.late_run, 4);
  CHECK_EQ(fast.min_interval, 0.25);
  rnf_backlog_policy composited = rnf_backlog_drain_policy(0, 0), vrr = rnf_backlog_drain_policy(1, 1);
  CHECK_EQ(composited.late_run, 60);
  CHECK_EQ(composited.min_interval, 5.0);
  CHECK_EQ(vrr.late_run, 60);
  CHECK_EQ(vrr.min_interval, 5.0);
}

TEST_CASE("present path needs every recent update direct") {
  const double r = 1.0 / 120;
  rnf_present_path* p = rnf_present_path_new();
  CHECK_FALSE(rnf_present_path_is_direct(p, r));
  for (int i = 0; i < RNF_PRESENT_PATH_WINDOW; ++i) rnf_present_path_observe(p, r);
  CHECK(rnf_present_path_is_direct(p, r));
  rnf_present_path_observe(p, 2 * r);
  CHECK_FALSE(rnf_present_path_is_direct(p, r));
  for (int i = 0; i < RNF_PRESENT_PATH_WINDOW - 1; ++i) rnf_present_path_observe(p, r);
  CHECK_FALSE(rnf_present_path_is_direct(p, r));
  rnf_present_path_observe(p, r);
  CHECK(rnf_present_path_is_direct(p, r));
  rnf_present_path_reset(p);
  CHECK_EQ(rnf_present_path_max_delay(p), 0.0);
  rnf_present_path_free(p);
}

// ------------------------------------------------------------------ frame pacing counters

TEST_CASE("steady presents have no judder") {
  rnf_frame_pacing* f = rnf_frame_pacing_new();
  for (int i = 0; i < 600; ++i) rnf_frame_pacing_present(f, uint64_t(i), double(i) * 2 / 120);
  CHECK_EQ(rnf_frame_pacing_presents(f), uint64_t(600));
  CHECK_EQ(rnf_frame_pacing_hitches(f), uint64_t(0));
  CHECK_EQ(rnf_frame_pacing_skipped(f), uint64_t(0));
  CHECK(near(rnf_frame_pacing_take_window_max(f), 2.0 / 120, 1e-9));
  CHECK_EQ(rnf_frame_pacing_take_window_max(f), 0.0);
  uint64_t frame = 0;
  double t = 0;
  REQUIRE(rnf_frame_pacing_last_present(f, &frame, &t));
  CHECK_EQ(frame, uint64_t(599));
  rnf_frame_pacing_free(f);
}

TEST_CASE("held and skipped frames are counted") {
  rnf_frame_pacing* f = rnf_frame_pacing_new();
  rnf_frame_pacing_present(f, 10, 1.0);
  rnf_frame_pacing_present(f, 11, 1.0 + 3.0 / 120);
  rnf_frame_pacing_present(f, 12, 1.0 + 4.0 / 120);
  rnf_frame_pacing_present(f, 14, 1.0 + 6.0 / 120);
  CHECK_EQ(rnf_frame_pacing_hitches(f), uint64_t(1));
  CHECK_EQ(rnf_frame_pacing_skipped(f), uint64_t(1));
  rnf_frame_pacing_free(f);
}

TEST_CASE("pauses and seeks are not pacing problems") {
  rnf_frame_pacing* f = rnf_frame_pacing_new();
  rnf_frame_pacing_present(f, 100, 1.0);
  rnf_frame_pacing_present(f, 101, 3.0);
  rnf_frame_pacing_present(f, 500, 3.0 + P);
  rnf_frame_pacing_present(f, 20, 3.0 + 2 * P);
  CHECK_EQ(rnf_frame_pacing_hitches(f), uint64_t(0));
  CHECK_EQ(rnf_frame_pacing_skipped(f), uint64_t(0));
  rnf_frame_pacing_free(f);
}

TEST_CASE("callback regularity") {
  rnf_callback_regularity* r = rnf_callback_regularity_new(1.5 * P, 0.25);
  double t = 0;
  for (int i = 0; i < 100; ++i) { rnf_callback_regularity_tick(r, t); t += P; }
  t += P;
  rnf_callback_regularity_tick(r, t);
  t += 5;
  rnf_callback_regularity_tick(r, t);
  CHECK_EQ(rnf_callback_regularity_count(r), uint64_t(102));
  CHECK_EQ(rnf_callback_regularity_late(r), uint64_t(1));
  CHECK(near(rnf_callback_regularity_take_window_max(r), 2 * P, 1e-9));
  rnf_callback_regularity_free(r);
}
