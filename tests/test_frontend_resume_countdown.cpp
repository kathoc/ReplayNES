// Frontend core: the resume countdown (docs/design/UI_REDESIGN.md, "Resume countdown"): 3, 2, 1
// over the paused picture before play resumes, on a fake clock; when it applies (record / practice
// playing, never replay, never stacked on the practice return); cancel, hold, the setting.
#include <cmath>
#include <vector>

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
