// Frontend core: the resume countdown (docs/design/UI_REDESIGN.md, "Resume countdown"): 3, 2, 1
// over the paused picture before play resumes, on a fake clock; when it applies (record / practice
// playing, never replay, never stacked on the practice return); cancel, hold, the setting; the
// type-ahead (docs/design/VTR_UX.md, "Countdown type-ahead"): NES buttons pressed during the
// countdown reach the first frame, on the fake clock and through rn_input into a recorded take.
#include <cmath>
#include <string>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

#include "replaynes/frontend.h"

namespace {

constexpr double kTick = 1.0 / 60;

struct Count {
  std::vector<int> counts;  // the numbers shown, in order (deduplicated)
  double seconds = 0;       // from the start to the done tick
  int doneTicks = 0;
};

// Ticks from `now` until done (or the limit).
Count runToEnd(rnf_resume_countdown* c, double& now, int limit = 1000) {
  Count r;
  double t0 = now;
  for (int i = 0; i < limit; ++i) {
    rnf_resume_countdown_state st = rnf_resume_countdown_tick(c, now);
    if (st.done) {
      r.doneTicks += 1;
      r.seconds = now - t0;
      CHECK_EQ(st.count, 0);
      break;
    }
    REQUIRE(st.count >= 1);
    REQUIRE(st.count <= 3);
    CHECK(st.fraction >= 0);
    CHECK(st.fraction <= 1);
    if (r.counts.empty() || r.counts.back() != st.count) r.counts.push_back(st.count);
    now += kTick;
  }
  return r;
}

}  // namespace

TEST_CASE("resume countdown: 3 2 1, one number per second, then done once") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  REQUIRE(c != nullptr);
  CHECK_EQ(rnf_resume_countdown_enabled(c), 1);  // on by default
  CHECK_EQ(rnf_resume_countdown_active(c), 0);
  rnf_resume_countdown_state idle = rnf_resume_countdown_tick(c, 5);
  CHECK_EQ(idle.count, 0);
  CHECK_EQ(idle.done, 0);

  double now = 100;
  CHECK_EQ(rnf_resume_countdown_start(c, now, RN_MODE_RECORD, RNF_PHASE_PLAYING), 1);
  CHECK_EQ(rnf_resume_countdown_active(c), 1);
  Count r = runToEnd(c, now);
  CHECK((r.counts == std::vector<int>{3, 2, 1}));
  CHECK(std::fabs(r.seconds - RNF_PRACTICE_COUNTDOWN_SECONDS) < 2 * kTick);
  CHECK_EQ(r.doneTicks, 1);
  CHECK_EQ(rnf_resume_countdown_active(c), 0);
  // Reported once: afterwards it is idle.
  rnf_resume_countdown_state after = rnf_resume_countdown_tick(c, now + kTick);
  CHECK_EQ(after.done, 0);
  CHECK_EQ(after.count, 0);
  rnf_resume_countdown_free(c);
}

TEST_CASE("resume countdown: the fraction runs 0..1 within each number (the practice countdown's look)") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  double t0 = 10;
  rnf_resume_countdown_start(c, t0, RN_MODE_RECORD, RNF_PHASE_PLAYING);
  rnf_resume_countdown_state a = rnf_resume_countdown_tick(c, t0);
  CHECK_EQ(a.count, 3);
  CHECK_EQ(a.fraction, 0.0);
  rnf_resume_countdown_state b = rnf_resume_countdown_tick(c, t0 + 1.5);
  CHECK_EQ(b.count, 2);
  CHECK(std::fabs(b.fraction - 0.5) < 1e-9);
  rnf_resume_countdown_state d = rnf_resume_countdown_tick(c, t0 + 2.25);
  CHECK_EQ(d.count, 1);
  CHECK(std::fabs(d.fraction - 0.25) < 1e-9);
  // Same numbers / fractions as the practice countdown at the same elapsed time.
  rnf_countdown_visual v = rnf_practice_countdown_visual(d.count, d.fraction);
  CHECK_EQ(v.number, 1);
  CHECK(v.alpha > 0.99f);
  rnf_resume_countdown_state e = rnf_resume_countdown_tick(c, t0 + 3.0);
  CHECK_EQ(e.done, 1);
  rnf_resume_countdown_free(c);
}

TEST_CASE("resume countdown: only for live play; never stacked on the practice return") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  CHECK_EQ(rnf_resume_countdown_wanted(c, RN_MODE_RECORD, RNF_PHASE_PLAYING), 1);
  CHECK_EQ(rnf_resume_countdown_wanted(c, RN_MODE_REPLAY, RNF_PHASE_PLAYING), 0);  // watching: no input to get ready
  CHECK_EQ(rnf_resume_countdown_wanted(c, RN_MODE_PRACTICE, RNF_PHASE_PLAYING), 1);
  CHECK_EQ(rnf_resume_countdown_wanted(c, RN_MODE_PRACTICE, RNF_PHASE_HOLDING), 0);
  CHECK_EQ(rnf_resume_countdown_wanted(c, RN_MODE_PRACTICE, RNF_PHASE_REWINDING), 0);
  CHECK_EQ(rnf_resume_countdown_wanted(c, RN_MODE_PRACTICE, RNF_PHASE_COUNTDOWN), 0);
  CHECK_EQ(rnf_resume_countdown_wanted(nullptr, RN_MODE_RECORD, RNF_PHASE_PLAYING), 1);

  CHECK_EQ(rnf_resume_countdown_start(c, 0, RN_MODE_REPLAY, RNF_PHASE_PLAYING), 0);
  CHECK_EQ(rnf_resume_countdown_active(c), 0);
  CHECK_EQ(rnf_resume_countdown_start(c, 0, RN_MODE_PRACTICE, RNF_PHASE_COUNTDOWN), 0);
  CHECK_EQ(rnf_resume_countdown_active(c), 0);
  CHECK_EQ(rnf_resume_countdown_start(c, 0, RN_MODE_PRACTICE, RNF_PHASE_PLAYING), 1);
  CHECK_EQ(rnf_resume_countdown_active(c), 1);
  // A later start that is not wanted clears it (e.g. resumed into replay).
  CHECK_EQ(rnf_resume_countdown_start(c, 1, RN_MODE_REPLAY, RNF_PHASE_PLAYING), 0);
  CHECK_EQ(rnf_resume_countdown_active(c), 0);
  CHECK_EQ(rnf_resume_countdown_start(nullptr, 0, RN_MODE_RECORD, RNF_PHASE_PLAYING), 0);
  rnf_resume_countdown_free(c);
}

TEST_CASE("resume countdown: the setting; off plays at once, also during a countdown") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  rnf_resume_countdown_set_enabled(c, 0);
  CHECK_EQ(rnf_resume_countdown_enabled(c), 0);
  CHECK_EQ(rnf_resume_countdown_wanted(c, RN_MODE_RECORD, RNF_PHASE_PLAYING), 0);
  CHECK_EQ(rnf_resume_countdown_start(c, 0, RN_MODE_RECORD, RNF_PHASE_PLAYING), 0);
  CHECK_EQ(rnf_resume_countdown_tick(c, 0).count, 0);
  rnf_resume_countdown_set_enabled(c, 1);
  CHECK_EQ(rnf_resume_countdown_start(c, 0, RN_MODE_RECORD, RNF_PHASE_PLAYING), 1);
  CHECK_EQ(rnf_resume_countdown_tick(c, 0.5).count, 3);
  rnf_resume_countdown_set_enabled(c, 0);
  CHECK_EQ(rnf_resume_countdown_active(c), 0);
  CHECK_EQ(rnf_resume_countdown_tick(c, 0.6).count, 0);
  rnf_resume_countdown_free(c);
}

TEST_CASE("resume countdown: independent of the practice countdown setting") {
  rnf_practice_loop* loop = rnf_practice_loop_new();
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  rnf_practice_loop_set_countdown(loop, 0);
  CHECK_EQ(rnf_resume_countdown_enabled(c), 1);
  CHECK_EQ(rnf_resume_countdown_start(c, 0, RN_MODE_PRACTICE, rnf_practice_loop_phase(loop, nullptr)), 1);
  rnf_resume_countdown_set_enabled(c, 0);
  CHECK_EQ(rnf_practice_loop_countdown(loop), 0);
  rnf_practice_loop_set_countdown(loop, 1);
  CHECK_EQ(rnf_resume_countdown_enabled(c), 0);
  rnf_resume_countdown_free(c);
  rnf_practice_loop_free(loop);
}

TEST_CASE("resume countdown: cancel (a pause, the menu, the cancel tap) and restart from 3") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  double now = 0;
  rnf_resume_countdown_start(c, now, RN_MODE_RECORD, RNF_PHASE_PLAYING);
  CHECK_EQ(rnf_resume_countdown_tick(c, now + 1.2).count, 2);
  rnf_resume_countdown_cancel(c);
  CHECK_EQ(rnf_resume_countdown_active(c), 0);
  rnf_resume_countdown_state st = rnf_resume_countdown_tick(c, now + 5);
  CHECK_EQ(st.count, 0);
  CHECK_EQ(st.done, 0);  // cancelled: never "done" (the frontend stays paused)
  // Resuming again starts over at 3 (time spent paused never counts).
  now = 50;
  rnf_resume_countdown_start(c, now, RN_MODE_RECORD, RNF_PHASE_PLAYING);
  Count r = runToEnd(c, now);
  CHECK((r.counts == std::vector<int>{3, 2, 1}));
  CHECK(std::fabs(r.seconds - RNF_PRACTICE_COUNTDOWN_SECONDS) < 2 * kTick);
  rnf_resume_countdown_free(c);
}

TEST_CASE("resume countdown: a modal UI holds it at its start") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  double now = 0;
  rnf_resume_countdown_start(c, now, RN_MODE_RECORD, RNF_PHASE_PLAYING);
  CHECK_EQ(rnf_resume_countdown_tick(c, 0.8).count, 3);
  for (int i = 0; i < 600; ++i) rnf_resume_countdown_hold(c, now += kTick);  // 10 s behind a dialog
  CHECK_EQ(rnf_resume_countdown_active(c), 1);
  Count r = runToEnd(c, now);
  CHECK((r.counts == std::vector<int>{3, 2, 1}));
  CHECK(std::fabs(r.seconds - RNF_PRACTICE_COUNTDOWN_SECONDS) < 2 * kTick);
  // Hold while idle does nothing.
  rnf_resume_countdown_hold(c, now);
  CHECK_EQ(rnf_resume_countdown_active(c), 0);
  rnf_resume_countdown_free(c);
}

TEST_CASE("resume countdown: clone is independent; NULL-safe") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  rnf_resume_countdown_start(c, 0, RN_MODE_RECORD, RNF_PHASE_PLAYING);
  rnf_resume_countdown* k = rnf_resume_countdown_clone(c);
  REQUIRE(k != nullptr);
  rnf_resume_countdown_cancel(c);
  CHECK_EQ(rnf_resume_countdown_active(k), 1);
  CHECK_EQ(rnf_resume_countdown_tick(k, 1.5).count, 2);
  rnf_resume_countdown_free(k);
  rnf_resume_countdown_free(c);
  CHECK_EQ(rnf_resume_countdown_clone(nullptr) == nullptr, true);
  rnf_resume_countdown_set_enabled(nullptr, 1);
  rnf_resume_countdown_hold(nullptr, 0);
  rnf_resume_countdown_cancel(nullptr);
  CHECK_EQ(rnf_resume_countdown_active(nullptr), 0);
  CHECK_EQ(rnf_resume_countdown_enabled(nullptr), 0);
  CHECK_EQ(rnf_resume_countdown_tick(nullptr, 0).count, 0);
  rnf_resume_countdown_free(nullptr);
}

// ------------------------------------------------------------------ type-ahead

namespace {

// Starts a countdown at 0 and feeds `samples` (one per counting tick, p1 only) at 1/60 s.
void countWith(rnf_resume_countdown* c, const std::vector<uint8_t>& samples) {
  REQUIRE(rnf_resume_countdown_start(c, 0, RN_MODE_RECORD, RNF_PHASE_PLAYING) == 1);
  double now = 0;
  for (uint8_t p1 : samples) {
    REQUIRE(rnf_resume_countdown_tick(c, now).count > 0);
    rnf_resume_countdown_input(c, p1, 0);
    now += kTick;
  }
}

bool finish(rnf_resume_countdown* c) { return rnf_resume_countdown_tick(c, 10).done == 1; }

}  // namespace

TEST_CASE("type-ahead: a tap during the countdown is pressed on the first frame, released on the next") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  countWith(c, {0, 0, RN_BTN_A, 0, 0});
  CHECK_EQ(rnf_resume_countdown_pending(c), 0);  // still counting
  uint8_t p1 = 0, p2 = 0;
  CHECK_EQ(rnf_resume_countdown_apply(c, &p1, &p2), 0);  // nothing before the end
  CHECK(finish(c));
  CHECK_EQ(rnf_resume_countdown_pending(c), 1);
  p1 = RN_BTN_UP;  // what the first frame samples
  CHECK_EQ(rnf_resume_countdown_apply(c, &p1, &p2), 1);
  CHECK_EQ(p1, RN_BTN_UP | RN_BTN_A);
  CHECK_EQ(p2, 0);
  CHECK_EQ(rnf_resume_countdown_pending(c), 0);
  p1 = 0;
  CHECK_EQ(rnf_resume_countdown_apply(c, &p1, &p2), 0);  // once: released on the next frame
  CHECK_EQ(p1, 0);
  rnf_resume_countdown_free(c);
}

TEST_CASE("type-ahead: every button of both pads; held ones need nothing added") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  REQUIRE(rnf_resume_countdown_start(c, 0, RN_MODE_RECORD, RNF_PHASE_PLAYING) == 1);
  rnf_resume_countdown_input(c, 0, 0);
  rnf_resume_countdown_input(c, RN_BTN_B | RN_BTN_SELECT | RN_BTN_START | RN_BTN_RIGHT, RN_BTN_A | RN_BTN_DOWN);
  rnf_resume_countdown_input(c, RN_BTN_RIGHT, 0);  // right still held, the rest tapped
  CHECK(finish(c));
  uint8_t p1 = RN_BTN_RIGHT, p2 = 0;  // right held into the first frame
  CHECK_EQ(rnf_resume_countdown_apply(c, &p1, &p2), 1);
  CHECK_EQ(p1, RN_BTN_B | RN_BTN_SELECT | RN_BTN_START | RN_BTN_RIGHT);
  CHECK_EQ(p2, RN_BTN_A | RN_BTN_DOWN);
  // Pressed during the countdown and held through: the sample already has it.
  countWith(c, {0, RN_BTN_LEFT, RN_BTN_LEFT});
  CHECK(finish(c));
  p1 = RN_BTN_LEFT;
  p2 = 0;
  CHECK_EQ(rnf_resume_countdown_apply(c, &p1, &p2), 0);
  CHECK_EQ(p1, RN_BTN_LEFT);
  rnf_resume_countdown_free(c);
}

TEST_CASE("type-ahead: the first sample is the baseline (a tap from before the countdown is not typed ahead)") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  // The seek bar's Play tap (B) is still latched in the first counting sample; the Playback button
  // that started it (A) is held on and then let go.
  countWith(c, {RN_BTN_B | RN_BTN_A, RN_BTN_A, 0, 0});
  CHECK(finish(c));
  CHECK_EQ(rnf_resume_countdown_pending(c), 0);
  uint8_t p1 = 0, p2 = 0;
  CHECK_EQ(rnf_resume_countdown_apply(c, &p1, &p2), 0);
  CHECK_EQ(p1, 0);
  // Let go and pressed again during the countdown: typed ahead.
  countWith(c, {RN_BTN_A, 0, RN_BTN_A, 0});
  CHECK(finish(c));
  CHECK_EQ(rnf_resume_countdown_apply(c, &p1, &p2), 1);
  CHECK_EQ(p1, RN_BTN_A);
  rnf_resume_countdown_free(c);
}

TEST_CASE("type-ahead: never both directions of an axis") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  // Left tapped, right held into the first frame: right only.
  countWith(c, {0, RN_BTN_LEFT, RN_BTN_RIGHT});
  CHECK(finish(c));
  uint8_t p1 = RN_BTN_RIGHT, p2 = 0;
  CHECK_EQ(rnf_resume_countdown_apply(c, &p1, &p2), 0);
  CHECK_EQ(p1, RN_BTN_RIGHT);
  // Up and down both tapped (pad 2), A too: neither direction, A stays.
  REQUIRE(rnf_resume_countdown_start(c, 0, RN_MODE_RECORD, RNF_PHASE_PLAYING) == 1);
  rnf_resume_countdown_input(c, 0, 0);
  rnf_resume_countdown_input(c, 0, RN_BTN_UP | RN_BTN_A);
  rnf_resume_countdown_input(c, 0, RN_BTN_DOWN);
  rnf_resume_countdown_input(c, 0, 0);
  CHECK(finish(c));
  p1 = 0;
  p2 = 0;
  CHECK_EQ(rnf_resume_countdown_apply(c, &p1, &p2), 1);
  CHECK_EQ(p1, 0);
  CHECK_EQ(p2, RN_BTN_A);
  rnf_resume_countdown_free(c);
}

TEST_CASE("type-ahead: cancel, restart and a modal hold clear it; switching the setting off keeps it") {
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  uint8_t p1 = 0, p2 = 0;
  // Cancelled (a pause, rewind held): nothing reaches a later frame.
  countWith(c, {0, RN_BTN_A, 0});
  rnf_resume_countdown_cancel(c);
  CHECK_EQ(rnf_resume_countdown_pending(c), 0);
  CHECK_EQ(rnf_resume_countdown_apply(c, &p1, &p2), 0);
  // Ended, then paused before the first frame: dropped too.
  countWith(c, {0, RN_BTN_A, 0});
  CHECK(finish(c));
  CHECK_EQ(rnf_resume_countdown_pending(c), 1);
  rnf_resume_countdown_cancel(c);
  CHECK_EQ(rnf_resume_countdown_pending(c), 0);
  // Restarted: starts empty.
  countWith(c, {0, RN_BTN_B, 0});
  countWith(c, {0, 0});
  CHECK(finish(c));
  CHECK_EQ(rnf_resume_countdown_pending(c), 0);
  // A modal UI holds it at its start: what came before starts over (with a new baseline).
  countWith(c, {0, RN_BTN_B, 0});
  rnf_resume_countdown_hold(c, 0.5);
  rnf_resume_countdown_input(c, RN_BTN_START, 0);  // baseline after the hold
  rnf_resume_countdown_input(c, 0, 0);
  CHECK(finish(c));
  CHECK_EQ(rnf_resume_countdown_pending(c), 0);
  // The setting switched off while counting: plays at once, with what was typed so far.
  countWith(c, {0, RN_BTN_SELECT, 0});
  rnf_resume_countdown_set_enabled(c, 0);
  CHECK_EQ(rnf_resume_countdown_active(c), 0);
  CHECK_EQ(rnf_resume_countdown_apply(c, &p1, &p2), 1);
  CHECK_EQ(p1, RN_BTN_SELECT);
  // Input while idle is ignored; NULL-safe.
  rnf_resume_countdown_input(c, RN_BTN_A, 0);
  CHECK_EQ(rnf_resume_countdown_pending(c), 0);
  rnf_resume_countdown_input(nullptr, 1, 1);
  CHECK_EQ(rnf_resume_countdown_apply(nullptr, &p1, &p2), 0);
  CHECK_EQ(rnf_resume_countdown_apply(c, nullptr, nullptr), 0);
  CHECK_EQ(rnf_resume_countdown_pending(nullptr), 0);
  rnf_resume_countdown_free(c);
}

TEST_CASE("type-ahead: through rn_input into the take (recorded, replayed identically)") {
  std::string tmp = rntest::tempDir("frontend-type-ahead");
  std::string rom = rntest::writeTestRom(tmp);
  rn_session_options o;
  rn_session_options_init(&o);
  rn_session* s = nullptr;
  REQUIRE(rn_session_new(rom.c_str(), nullptr, &o, &s) == RN_OK);
  REQUIRE(rn_set_mode(s, RN_MODE_RECORD) == RN_OK);
  rn_input* in = rn_input_new();
  rn_input_bind(in, "kb:7", "p1.a");
  rn_input_bind(in, "kb:6", "p1.b");
  rn_input_bind(in, "kb:124", "p1.right");
  rn_input_bind(in, "kb:36", "p1.start");

  // Live play: the frontend's frame (sample, apply, rn_step).
  rnf_resume_countdown* c = rnf_resume_countdown_new();
  auto liveFrame = [&] {
    uint8_t p1 = 0, p2 = 0;
    rn_input_sample_game(in, rn_frame(s), &p1, &p2);
    rnf_resume_countdown_apply(c, &p1, &p2);
    REQUIRE(rn_step(s, p1, p2, 0, nullptr) == RN_OK);
  };
  for (int i = 0; i < 10; ++i) liveFrame();
  // Paused: the seek bar's Play tap (B) reaches the game while paused (nothing samples it).
  rn_input_set_pressed(in, "kb:6", 1);
  rn_input_set_pressed(in, "kb:6", 0);
  // The countdown: the game input is sampled every tick and handed over; nothing is emulated.
  double now = 0;
  REQUIRE(rnf_resume_countdown_start(c, now, RN_MODE_RECORD, RNF_PHASE_PLAYING) == 1);
  for (int tick = 0;; ++tick, now += kTick) {
    if (tick == 30) {  // A tapped
      rn_input_set_pressed(in, "kb:7", 1);
      rn_input_set_pressed(in, "kb:7", 0);
    }
    if (tick == 60) rn_input_set_pressed(in, "kb:36", 1);   // START...
    if (tick == 61) rn_input_set_pressed(in, "kb:36", 0);   // ...let go
    if (tick == 150) rn_input_set_pressed(in, "kb:124", 1);  // right, held on
    if (rnf_resume_countdown_tick(c, now).done) break;
    uint8_t p1 = 0, p2 = 0;
    rn_input_sample_game(in, rn_frame(s), &p1, &p2);
    rnf_resume_countdown_input(c, p1, p2);
  }
  CHECK_EQ(rn_frame(s), 10u);  // nothing emulated during the countdown
  liveFrame();                 // the first frame, in the done tick
  rn_input_set_pressed(in, "kb:124", 0);
  liveFrame();
  liveFrame();
  CHECK_EQ(rn_take_length(s), 13u);

  // Recorded: frame 10 = A + START (typed ahead) + right (held); B, the Play tap from before the
  // countdown, is not. 11: right let go, the taps released.
  REQUIRE(rn_set_mode(s, RN_MODE_REPLAY) == RN_OK);
  REQUIRE(rn_seek(s, 10) == RN_OK);
  const uint8_t want[3] = {RN_BTN_A | RN_BTN_START | RN_BTN_RIGHT, 0, 0};
  for (uint8_t w : want) {
    rn_step_info info{};
    REQUIRE(rn_step(s, 0, 0, 0, &info) == RN_OK);
    CHECK_EQ(info.p1, w);
    CHECK_EQ(info.p2, 0);
  }
  rnf_resume_countdown_free(c);
  rn_input_free(in);
  rn_session_close(s);
}
