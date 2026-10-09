// Frontend core: the practice return to A (docs/design/UI_REDESIGN.md, "Practice: return to A"):
// hold -> 1 s VTR rewind (whatever the section length) -> countdown 3, 2, 1 -> play; the L2 + R2
// chord; input gating; pauses not counted; the reel (adaptively decimated pictures for the sweep);
// the VTR effect (fades, flash-reduction toning, photosensitivity limits); the picture at A; the
// seek bar's Y (practice) / X (next slot, delete marker).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {

constexpr double kTick = 1.0 / 60;

struct Run {
  std::vector<rnf_practice_action> actions;
  double holdSeconds = 0, rewindSeconds = 0, countdownSeconds = 0;
  std::vector<int> counts;  // countdown numbers in order (deduplicated)
};

// Plays a loop to B and through one full return; times each phase on the fake clock.
Run runOnce(rnf_practice_loop* loop, uint64_t length, double& now) {
  Run r;
  uint64_t counter = 0;
  rnf_practice_action a{};
  for (;;) {
    a = rnf_practice_loop_tick(loop, now, counter, 1, length);
    if (a.kind != RNF_PRACTICE_STEP) break;
    ++counter;
    now += kTick;
  }
  REQUIRE_EQ(a.kind, RNF_PRACTICE_BEGIN_HOLD);
  double t0 = now;
  while (a.kind != RNF_PRACTICE_REWIND_FRAME) {
    now += kTick;
    a = rnf_practice_loop_tick(loop, now, counter, 1, length);
  }
  r.holdSeconds = now - t0;
  t0 = now;
  while (a.kind == RNF_PRACTICE_REWIND_FRAME) {
    r.actions.push_back(a);
    now += kTick;
    a = rnf_practice_loop_tick(loop, now, counter, 1, length);
  }
  r.rewindSeconds = now - t0;
  REQUIRE_EQ(a.kind, RNF_PRACTICE_RESTART);
  counter = 0;  // the frontend went to A
  t0 = now;
  for (;;) {
    now += kTick;
    a = rnf_practice_loop_tick(loop, now, counter, 1, length);
    if (a.kind != RNF_PRACTICE_COUNTDOWN) break;
    if (r.counts.empty() || r.counts.back() != a.count) r.counts.push_back(a.count);
  }
  r.countdownSeconds = now - t0;
  CHECK_EQ(a.kind, RNF_PRACTICE_STEP);
  return r;
}

std::vector<uint32_t> picture(uint32_t (*f)(int x, int y)) {
  std::vector<uint32_t> v(size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT);
  for (int y = 0; y < RN_VIDEO_HEIGHT; ++y)
    for (int x = 0; x < RN_VIDEO_WIDTH; ++x) v[size_t(y) * RN_VIDEO_WIDTH + size_t(x)] = f(x, y);
  return v;
}

double luma(uint32_t p) {
  return 0.299 * double((p >> 16) & 0xFF) + 0.587 * double((p >> 8) & 0xFF) + 0.114 * double(p & 0xFF);
}
double meanLuma(const std::vector<uint32_t>& v) {
  double s = 0;
  for (uint32_t p : v) s += luma(p);
  return s / double(v.size());
}

rn_session* newSession(std::string& tmp) {
  tmp = rntest::tempDir("frontend-practice-return");
  std::string rom = rntest::writeTestRom(tmp);
  rn_session_options o;
  rn_session_options_init(&o);
  rn_session* s = nullptr;
  rn_session_new(rom.c_str(), nullptr, &o, &s);
  rn_set_mode(s, RN_MODE_RECORD);
  return s;
}

}  // namespace

TEST_CASE("practice return: hold, then a 1 s rewind whatever the length, then 3 2 1, then play") {
  for (uint64_t length : {uint64_t(12), uint64_t(600), uint64_t(20000)}) {
    rnf_practice_loop* loop = rnf_practice_loop_new();
    CHECK_EQ(rnf_practice_loop_countdown(loop), 1);  // on by default
    double now = 100;
    Run r = runOnce(loop, length, now);
    CHECK(std::fabs(r.holdSeconds - RNF_PRACTICE_HOLD_SECONDS) < 2 * kTick);
    CHECK(std::fabs(r.rewindSeconds - 1.0) < 2 * kTick);  // independent of the length
    CHECK(std::fabs(r.countdownSeconds - 3.0) < 2 * kTick);
    CHECK((r.counts == std::vector<int>{3, 2, 1}));
    // The sweep runs evenly from the newest (0) to A (-> 1).
    REQUIRE(r.actions.size() >= 55);
    CHECK_EQ(r.actions.front().back, 0.0);
    for (size_t i = 1; i < r.actions.size(); ++i) CHECK(r.actions[i].back > r.actions[i - 1].back);
    CHECK(r.actions.back().back > 0.95);
    CHECK_EQ(rnf_practice_loop_loops(loop), 1);
    rnf_practice_loop_free(loop);
  }
}

TEST_CASE("practice return: the countdown is optional; its fraction runs 0..1 per number") {
  rnf_practice_loop* loop = rnf_practice_loop_new();
  rnf_practice_loop_set_countdown(loop, 0);
  double now = 0;
  uint64_t counter = 10;
  rnf_practice_action a = rnf_practice_loop_tick(loop, now, counter, 1, 10);
  CHECK_EQ(a.kind, RNF_PRACTICE_BEGIN_HOLD);
  while (a.kind != RNF_PRACTICE_RESTART) a = rnf_practice_loop_tick(loop, now += kTick, counter, 1, 10);
  CHECK_EQ(rnf_practice_loop_tick(loop, now += kTick, 0, 1, 10).kind, RNF_PRACTICE_STEP);
  rnf_practice_loop_free(loop);

  rnf_practice_loop* l2 = rnf_practice_loop_new();
  CHECK_EQ(rnf_practice_loop_return(l2, 0), 1);
  now = 0;
  while (rnf_practice_loop_tick(l2, now += kTick, 5, 0, 0).kind != RNF_PRACTICE_RESTART) {}
  double last = -1;
  int lastCount = 4;
  for (int i = 0; i < 170; ++i) {
    a = rnf_practice_loop_tick(l2, now += kTick, 0, 0, 0);
    REQUIRE_EQ(a.kind, RNF_PRACTICE_COUNTDOWN);
    if (a.count != lastCount) { last = -1; lastCount = a.count; }
    CHECK(a.fraction >= 0 && a.fraction <= 1 && a.fraction > last);
    last = a.fraction;
  }
  // Switching it off during the countdown plays at once.
  rnf_practice_loop_set_countdown(l2, 0);
  CHECK_EQ(rnf_practice_loop_phase(l2, nullptr), RNF_PHASE_PLAYING);
  rnf_practice_loop_free(l2);
}

TEST_CASE("practice return: L2 + R2 returns at once; latched until both are released; no rewind meanwhile") {
  rnf_practice_loop* l = rnf_practice_loop_new();
  double now = 0;
  int rewind = -1;
  // L2 alone: rewinds the run as before.
  CHECK_EQ(rnf_practice_loop_shoulders(l, now, 1, 1, 0, &rewind), 0);
  CHECK_EQ(rewind, 1);
  // R2 joins: the chord fires once, from the middle of the section (no hold).
  CHECK_EQ(rnf_practice_loop_shoulders(l, now += kTick, 1, 1, 1, &rewind), 1);
  CHECK_EQ(rewind, 0);
  CHECK_EQ(rnf_practice_loop_phase(l, nullptr), RNF_PHASE_REWINDING);
  CHECK_EQ(rnf_practice_loop_tick(l, now, 37, 1, 600).kind, RNF_PRACTICE_REWIND_FRAME);
  CHECK_EQ(rnf_practice_loop_shoulders(l, now += kTick, 1, 1, 1, &rewind), 0);  // still held: once
  CHECK_EQ(rnf_practice_loop_shoulders(l, now += kTick, 1, 1, 0, &rewind), 0);  // R2 up, L2 still held
  CHECK_EQ(rewind, 0);
  // Released: the return goes on; L2 during the sweep / countdown does nothing.
  CHECK_EQ(rnf_practice_loop_shoulders(l, now += kTick, 1, 0, 0, &rewind), 0);
  CHECK_EQ(rnf_practice_loop_shoulders(l, now += kTick, 1, 1, 0, &rewind), 0);
  CHECK_EQ(rewind, 0);
  // A chord during the return is ignored (no restart of the sweep).
  CHECK_EQ(rnf_practice_loop_shoulders(l, now += kTick, 1, 1, 1, &rewind), 0);
  double since = 0;
  rnf_practice_loop_phase(l, &since);
  CHECK(since < 0.1);
  rnf_practice_loop_free(l);

  // From the hold at B: straight into the rewind.
  rnf_practice_loop* h = rnf_practice_loop_new();
  CHECK_EQ(rnf_practice_loop_tick(h, 0, 50, 1, 50).kind, RNF_PRACTICE_BEGIN_HOLD);
  CHECK_EQ(rnf_practice_loop_shoulders(h, 0.1, 1, 1, 1, nullptr), 1);
  CHECK_EQ(rnf_practice_loop_phase(h, nullptr), RNF_PHASE_REWINDING);
  rnf_practice_loop_free(h);

  // No A (free practice without a slot): the chord does nothing, but still latches.
  rnf_practice_loop* n = rnf_practice_loop_new();
  CHECK_EQ(rnf_practice_loop_shoulders(n, 0, 0, 1, 1, &rewind), 0);
  CHECK_EQ(rewind, 0);
  CHECK_EQ(rnf_practice_loop_phase(n, nullptr), RNF_PHASE_PLAYING);
  rnf_practice_loop_free(n);
}

TEST_CASE("practice return: time not ticking (paused, the menu) is not counted") {
  rnf_practice_loop* l = rnf_practice_loop_new();
  double now = 0;
  rnf_practice_loop_return(l, now);
  while (rnf_practice_loop_tick(l, now += kTick, 3, 1, 100).kind != RNF_PRACTICE_RESTART) {}
  rnf_practice_action a{};
  for (int i = 0; i < 70; ++i) a = rnf_practice_loop_tick(l, now += kTick, 0, 1, 100);
  REQUIRE_EQ(a.kind, RNF_PRACTICE_COUNTDOWN);
  CHECK_EQ(a.count, 2);
  double f = a.fraction;
  now += 30;  // paused half a minute
  a = rnf_practice_loop_tick(l, now, 0, 1, 100);
  CHECK_EQ(a.kind, RNF_PRACTICE_COUNTDOWN);
  CHECK_EQ(a.count, 2);
  CHECK(std::fabs(a.fraction - f) < 2 * kTick);
  rnf_practice_loop_free(l);
}

TEST_CASE("countdown visual: fade / scale in, hold, fade out; ring runs down") {
  rnf_countdown_visual v0 = rnf_practice_countdown_visual(3, 0);
  CHECK_EQ(v0.number, 3);
  CHECK(v0.alpha < 0.05f);
  CHECK(v0.scale > 1.05f && v0.scale < 1.2f);
  rnf_countdown_visual v1 = rnf_practice_countdown_visual(2, 0.5);
  CHECK_EQ(v1.alpha, 1.0f);
  CHECK_EQ(v1.scale, 1.0f);
  CHECK(std::fabs(v1.ring - 0.5f) < 1e-6f);
  rnf_countdown_visual v2 = rnf_practice_countdown_visual(1, 0.999);
  CHECK(v2.alpha < 0.05f);
  CHECK(v2.scale < 1.0f && v2.scale > 0.9f);
  CHECK_EQ(rnf_practice_countdown_visual(0, 0.5).number, 0);
  // Smooth: no jump between neighbouring display frames.
  float prev = rnf_practice_countdown_visual(2, 0).alpha;
  for (int i = 1; i <= 60; ++i) {
    float a = rnf_practice_countdown_visual(2, i / 60.0).alpha;
    CHECK(std::fabs(a - prev) < 0.35f);
    prev = a;
  }
}

TEST_CASE("reel: evenly spread from A, decimated when full, the first frame after A always kept") {
  std::vector<uint32_t> f(size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT);
  rnf_reel* r = rnf_reel_new(RNF_REEL_CAPACITY);
  CHECK_EQ(rnf_reel_count(r), 0);
  CHECK_EQ(rnf_reel_sweep_index(r, 0.5), -1);
  for (uint64_t p = 1; p <= 1000; ++p) {
    f[0] = uint32_t(p);
    rnf_reel_offer(r, p, f.data());
    int n = rnf_reel_count(r);
    REQUIRE(n <= RNF_REEL_CAPACITY);
    if (p >= 120) REQUIRE(n >= RNF_REEL_CAPACITY / 2);
  }
  uint64_t stride = rnf_reel_stride(r);
  CHECK_EQ(stride, uint64_t(16));  // 1000 frames: 1, 2, 4, 8, 16
  int n = rnf_reel_count(r);
  CHECK_EQ(rnf_reel_position(r, 0), uint64_t(1));
  for (int i = 0; i < n; ++i) {
    uint64_t p = rnf_reel_position(r, i);
    CHECK_EQ((p - 1) % stride, uint64_t(0));
    if (i > 0) CHECK_EQ(p - rnf_reel_position(r, i - 1), stride);
    CHECK_EQ(uint64_t(rnf_reel_frame(r, i)[0]), p);  // the picture of that position
  }
  // Sweep: newest at 0, A's neighbour at 1.
  CHECK_EQ(rnf_reel_sweep_index(r, 0), n - 1);
  CHECK_EQ(rnf_reel_sweep_index(r, 1), 0);
  CHECK_EQ(rnf_reel_sweep_index(r, 7), 0);
  // Rewinding the run drops the future; offering continues on the grid.
  rnf_reel_truncate(r, 490);
  CHECK_EQ(rnf_reel_position(r, rnf_reel_count(r) - 1), uint64_t(481));
  CHECK_EQ(rnf_reel_offer(r, 497, f.data()), 1);  // (497 - 1) % 16 == 0, after the kept 481
  CHECK_EQ(rnf_reel_offer(r, 497, f.data()), 0);  // not twice
  CHECK_EQ(rnf_reel_offer(r, 498, f.data()), 0);  // off the grid
  rnf_reel_clear(r);
  CHECK_EQ(rnf_reel_count(r), 0);
  CHECK_EQ(rnf_reel_stride(r), uint64_t(1));
  // A short section keeps every frame.
  for (uint64_t p = 1; p <= 40; ++p) rnf_reel_offer(r, p, f.data());
  CHECK_EQ(rnf_reel_count(r), 40);
  rnf_reel_release(r);
  CHECK_EQ(rnf_reel_count(r), 0);
  CHECK(rnf_reel_frame(r, 0) == nullptr);
  rnf_reel_free(r);
}

TEST_CASE("VTR effect: fades in / out, zero when off or disabled, toned down by flash reduction") {
  rnf_vtr* v = rnf_vtr_new();
  rnf_vtr_configure(v, 1, RN_FLASH_OFF);
  double now = 0;
  rnf_vtr_params p = rnf_vtr_tick(v, now, RNF_VTR_NONE);
  CHECK_EQ(p.strength, 0.0f);
  CHECK_EQ(rnf_vtr_active(v), 0);
  int ticks = 0;
  do {
    p = rnf_vtr_tick(v, now += kTick, RNF_VTR_RETURN);
    ++ticks;
  } while (p.strength < rnf_vtr_peak(v, RNF_VTR_RETURN) && ticks < 100);
  CHECK(ticks <= int(RNF_VTR_FADE_IN_SECONDS * 60) + 1);
  CHECK_EQ(p.kind, RNF_VTR_RETURN);
  ticks = 0;
  do {
    p = rnf_vtr_tick(v, now += kTick, RNF_VTR_NONE);
    ++ticks;
  } while (p.strength > 0 && ticks < 100);
  CHECK(ticks <= int(RNF_VTR_FADE_OUT_SECONDS * 60) + 1);
  CHECK_EQ(rnf_vtr_active(v), 0);
  // Reduce Flashing tones it down; the setting turns it off.
  float off = rnf_vtr_peak(v, RNF_VTR_REWIND);
  rnf_vtr_configure(v, 1, RN_FLASH_STANDARD);
  float standard = rnf_vtr_peak(v, RNF_VTR_REWIND);
  rnf_vtr_configure(v, 1, RN_FLASH_HIGH);
  float high = rnf_vtr_peak(v, RNF_VTR_REWIND);
  CHECK(off > standard);
  CHECK(standard > high);
  CHECK(high > 0);
  CHECK(rnf_vtr_peak(v, RNF_VTR_FAST_FORWARD) <= rnf_vtr_peak(v, RNF_VTR_REWIND));
  CHECK(rnf_vtr_peak(v, RNF_VTR_REWIND) < rnf_vtr_peak(v, RNF_VTR_RETURN));
  rnf_vtr_configure(v, 0, RN_FLASH_OFF);
  for (int i = 0; i < 30; ++i) CHECK_EQ(rnf_vtr_tick(v, now += kTick, RNF_VTR_RETURN).strength, 0.0f);
  rnf_vtr_free(v);

  // Strength 0 is the identity.
  std::vector<uint32_t> in = picture([](int x, int y) { return 0xFF000000u | uint32_t((x * 7 + y * 3) & 0xFFFFFF); });
  std::vector<uint32_t> out(in.size());
  rnf_vtr_params zero{};
  rnf_vtr_apply(in.data(), out.data(), &zero);
  CHECK(out == in);
}

TEST_CASE("VTR effect: photosensitivity - no large-area flashes, small mean change, smooth bands") {
  struct Pic { const char* name; std::vector<uint32_t> px; };
  std::vector<Pic> pics = {
      {"black", picture([](int, int) { return 0xFF000000u; })},
      {"white", picture([](int, int) { return 0xFFFFFFFFu; })},
      {"grey", picture([](int, int) { return 0xFF808080u; })},
      {"sky", picture([](int, int y) { return y < 200 ? 0xFF5C94FCu : 0xFFC84C0Cu; })},  // SMB-like
      {"checker", picture([](int x, int y) { return ((x / 8 + y / 8) & 1) ? 0xFFFFFFFFu : 0xFF000000u; })},
  };
  for (rnf_vtr_kind kind : {RNF_VTR_RETURN, RNF_VTR_REWIND, RNF_VTR_FAST_FORWARD}) {
    for (const Pic& pic : pics) {
      std::vector<uint32_t> prev, out(pic.px.size());
      const double base = meanLuma(pic.px);
      double worstMean = 0, worstArea = 0, worstStep = 0;
      for (int i = 0; i < 120; ++i) {
        rnf_vtr_params p{1.0f, i * kTick, kind};  // full strength (Reduce Flashing off): the worst case
        rnf_vtr_apply(pic.px.data(), out.data(), &p);
        worstMean = std::max(worstMean, std::fabs(meanLuma(out) - base));
        if (!prev.empty()) {
          // Pixels whose luminance jumps by more than 10 % between two frames: a small area only.
          size_t changed = 0;
          for (size_t k = 0; k < out.size(); ++k)
            if (std::fabs(luma(out[k]) - luma(prev[k])) > 25.5) ++changed;
          worstArea = std::max(worstArea, double(changed) / double(out.size()));
          worstStep = std::max(worstStep, std::fabs(meanLuma(out) - meanLuma(prev)));
        }
        prev = out;
      }
      if (getenv("VTR_STATS") || worstMean >= 0.05 * 255 || worstStep >= 0.01 * 255 || worstArea >= 0.08)
        std::fprintf(stderr, "  %s kind %d: mean %.2f step %.2f area %.4f\n", pic.name, int(kind), worstMean, worstStep, worstArea);
      CHECK(worstMean < 0.05 * 255);    // the picture's overall brightness barely moves
      CHECK(worstStep < 0.01 * 255);    // and never jumps from one frame to the next
      CHECK(worstArea < 0.08);          // what flickers is small (WCAG flash area: 25 %)
    }
  }
}

TEST_CASE("VTR effect: the passes in a frame are cheap enough to run every tick (< 2 ms)") {
  std::vector<uint32_t> in = picture([](int x, int y) { return 0xFF000000u | uint32_t((x * 0x010203 + y * 0x030201) & 0xFFFFFF); });
  std::vector<uint32_t> out(in.size());
  rnf_vtr_params p{1.0f, 0.5, RNF_VTR_RETURN};
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 60; ++i) {
    p.time = i * kTick;
    rnf_vtr_apply(in.data(), out.data(), &p);
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 60;
  std::fprintf(stderr, "  vtr pass: %.3f ms\n", ms);
  CHECK(ms < 2.0);
}

TEST_CASE("picture at A: the countdown's picture without moving the run") {
  std::string tmp;
  rn_session* s = newSession(tmp);
  for (int i = 0; i < 40; ++i) rn_step(s, uint8_t(i % 20 < 9 ? RN_BTN_RIGHT : 0), 0, 0, nullptr);
  REQUIRE_EQ(rn_practice_set_a(s, 0), RN_OK);
  for (int i = 0; i < 60; ++i) rn_step(s, RN_BTN_RIGHT, 0, 0, nullptr);
  REQUIRE_EQ(rn_practice_set_b(s, 0), RN_OK);
  CHECK_EQ(rnf_practice_preview_a(s), RN_ERR_WRONG_MODE);  // not practicing
  REQUIRE_EQ(rn_practice_goto_a(s, 0), RN_OK);
  const uint64_t hashA = rn_state_hash(s);
  // The frame after A with no input, then back.
  rn_step(s, 0, 0, 0, nullptr);
  const uint64_t expected = rn_video_hash(s);
  REQUIRE_EQ(rn_practice_goto_a(s, 0), RN_OK);
  REQUIRE_EQ(rnf_practice_preview_a(s), RN_OK);
  CHECK_EQ(rn_video_hash(s), expected);
  CHECK_EQ(rn_practice_frame(s), uint64_t(0));
  CHECK_EQ(rn_state_hash(s), hashA);
  rn_practice_slot_info si{};
  rn_practice_slot_get(s, 0, &si);
  CHECK_EQ(si.length_frames, uint64_t(60));
  // Playing on is exactly as without the preview (determinism).
  rn_step(s, RN_BTN_RIGHT, 0, 0, nullptr);
  const uint64_t withPreview = rn_state_hash(s);
  REQUIRE_EQ(rn_practice_goto_a(s, 0), RN_OK);
  rn_step(s, RN_BTN_RIGHT, 0, 0, nullptr);
  CHECK_EQ(rn_state_hash(s), withPreview);
  rn_session_close(s);
}

TEST_CASE("seek bar Y / X: Y practices a slot with a section, X cycles the slot or deletes the focused marker") {
  rnf_markers* m = rnf_markers_new();
  rnf_markers_sync(m, nullptr, 1000);
  CHECK_EQ(rnf_markers_face(m, 1), RNF_SEEK_FACE_NONE);       // empty slot: nothing to practice
  CHECK_EQ(rnf_markers_face(m, 0), RNF_SEEK_FACE_NEXT_SLOT);  // X on the bar: the next A/B slot
  rnf_timeline_range aOnly{0, 100, 0, 0};
  rnf_markers_sync(m, &aOnly, 1000);
  CHECK_EQ(rnf_markers_face(m, 1), RNF_SEEK_FACE_PRACTICE);  // A only: practice without a loop
  rnf_timeline_range ab{0, 100, 1, 300};
  rnf_markers_sync(m, &ab, 1000);
  CHECK_EQ(rnf_markers_face(m, 1), RNF_SEEK_FACE_PRACTICE);
  CHECK_EQ(rnf_markers_face(m, 0), RNF_SEEK_FACE_NEXT_SLOT);
  rnf_markers_move(m, 0, -1, 120);  // up: a marker has the focus
  REQUIRE(rnf_markers_focus(m) >= 0);
  CHECK_EQ(rnf_markers_face(m, 0), RNF_SEEK_FACE_DELETE_MARKER);
  CHECK_EQ(rnf_markers_face(m, 1), RNF_SEEK_FACE_PRACTICE);
  rnf_markers_confirm(m, 120);  // editing: neither
  REQUIRE(rnf_markers_editing(m));
  CHECK_EQ(rnf_markers_face(m, 0), RNF_SEEK_FACE_NONE);
  CHECK_EQ(rnf_markers_face(m, 1), RNF_SEEK_FACE_NONE);
  rnf_markers_free(m);
}
