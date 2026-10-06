// Frontend core: transport / practice state machines (record toggle, slow toggle, paused step
// repeat, practice A/B loop on a fake clock and on a real session, fast-forward over the take),
// the frame history ring and the audio fade tail. Ported from the macOS PlaybackLogicTests.
#include <set>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {

rn_session* newSession(std::string& tmp) {
  tmp = rntest::tempDir("frontend-playback");
  std::string rom = rntest::writeTestRom(tmp);
  rn_session_options o;
  rn_session_options_init(&o);
  rn_session* s = nullptr;
  rn_session_new(rom.c_str(), nullptr, &o, &s);
  rn_set_mode(s, RN_MODE_RECORD);
  return s;
}

void record(rn_session* s, int n, int seed = 0) {
  for (int i = 0; i < n; ++i) {
    int f = i + seed;
    uint8_t p1 = 0;
    if (f % 30 < 12) p1 |= RN_BTN_RIGHT;
    if (f % 17 == 0) p1 |= RN_BTN_A;
    rn_step(s, p1, 0, 0, nullptr);
  }
}

bool operator==(const rnf_record_toggle_plan& a, const rnf_record_toggle_plan& b) {
  return a.record == b.record && a.has_seek == b.has_seek && (!a.has_seek || a.seek == b.seek) && a.play == b.play;
}

}  // namespace

TEST_CASE("slow toggle") {
  CHECK_EQ(rnf_slow_toggled(1), 2);
  CHECK_EQ(rnf_slow_toggled(2), 1);
  CHECK_EQ(rnf_slow_toggled(4), 1);
  CHECK_EQ(take(rnf_slow_label(1)), "Normal");
  CHECK_EQ(take(rnf_slow_label(2)), "1/2");
  CHECK_EQ(take(rnf_slow_label(4)), "1/4");
}

TEST_CASE("step repeater") {
  rnf_step_repeater* r = rnf_step_repeater_new();
  CHECK_EQ(rnf_step_repeater_press(r, -1), -1);
  std::vector<int> steps;
  for (int t = 1; t <= 30; ++t) if (rnf_step_repeater_tick(r) != 0) steps.push_back(t);
  REQUIRE(!steps.empty());
  CHECK_EQ(steps.front(), RNF_STEP_REPEAT_INITIAL_DELAY);
  std::vector<int> want;
  for (int t = RNF_STEP_REPEAT_INITIAL_DELAY; t <= 30; t += RNF_STEP_REPEAT_INTERVAL) want.push_back(t);
  CHECK(steps == want);
  rnf_step_repeater_release(r, -1);
  CHECK_EQ(rnf_step_repeater_tick(r), 0);
  CHECK_EQ(rnf_step_repeater_press(r, 1), 1);
  rnf_step_repeater_release(r, -1);
  CHECK_EQ(rnf_step_repeater_direction(r), 1);
  rnf_step_repeater* copy = rnf_step_repeater_clone(r);
  rnf_step_repeater_reset(r);
  CHECK_EQ(rnf_step_repeater_direction(copy), 1);
  rnf_step_repeater_free(copy);
  rnf_step_repeater_free(r);
}

TEST_CASE("record toggle state machine") {
  CHECK((rnf_record_toggle(1, 300, 300) == rnf_record_toggle_plan{0, 1, 0, 1}));
  CHECK((rnf_record_toggle(1, 120, 300) == rnf_record_toggle_plan{0, 0, 0, 1}));
  CHECK_EQ(rnf_record_toggle(1, 0, 0).play, 0);
  CHECK((rnf_record_toggle(0, 150, 300) == rnf_record_toggle_plan{1, 0, 0, 0}));
  CHECK(rnf_record_toggle_restarts_on_play(0, 0, 300, 300));
  CHECK_FALSE(rnf_record_toggle_restarts_on_play(1, 0, 300, 300));
  CHECK_FALSE(rnf_record_toggle_restarts_on_play(0, 1, 300, 300));
  CHECK_FALSE(rnf_record_toggle_restarts_on_play(0, 0, 10, 300));
}

TEST_CASE("practice loop state machine on a fake clock") {
  rnf_practice_loop* loop = rnf_practice_loop_new();
  double now = 0;
  uint64_t counter = 0;
  const uint64_t length = 10;
  int stepsTaken = 0;
  rnf_practice_action a{};
  for (;;) {
    a = rnf_practice_loop_tick(loop, now, counter, 1, length);
    if (a.kind != RNF_PRACTICE_STEP) break;
    ++stepsTaken;
    ++counter;
    now += 1.0 / 60;
  }
  CHECK_EQ(stepsTaken, 10);
  CHECK_EQ(a.kind, RNF_PRACTICE_BEGIN_HOLD);
  const double holdStart = now;
  do {
    now += 1.0 / 60;
    a = rnf_practice_loop_tick(loop, now, counter, 1, length);
  } while (a.kind == RNF_PRACTICE_HOLD);
  CHECK(now - holdStart >= RNF_PRACTICE_HOLD_SECONDS);
  CHECK(a.kind == RNF_PRACTICE_REWIND_FRAME && a.back == 0);
  const double rwStart = now;
  double lastBack = -1;
  for (;;) {
    now += 1.0 / 60;
    a = rnf_practice_loop_tick(loop, now, counter, 1, length);
    if (a.kind != RNF_PRACTICE_REWIND_FRAME) break;
    CHECK(a.back > lastBack);
    lastBack = a.back;
  }
  CHECK_EQ(a.kind, RNF_PRACTICE_RESTART);
  CHECK(now - rwStart >= RNF_PRACTICE_REWIND_SECONDS);
  CHECK_EQ(rnf_practice_loop_loops(loop), 1);
  CHECK_EQ(rnf_practice_loop_tick(loop, now, 0, 1, length).kind, RNF_PRACTICE_STEP);
  rnf_practice_loop_free(loop);

  rnf_practice_loop* free = rnf_practice_loop_new();
  CHECK_EQ(rnf_practice_loop_tick(free, 0, 9999, 0, 0).kind, RNF_PRACTICE_STEP);
  rnf_practice_loop_free(free);
  rnf_practice_loop* l2 = rnf_practice_loop_new();
  rnf_practice_loop_tick(l2, 0, 10, 1, 10);
  CHECK(rnf_practice_loop_phase(l2, nullptr) != RNF_PHASE_PLAYING);
  rnf_practice_loop_interrupt(l2);
  CHECK_EQ(rnf_practice_loop_tick(l2, 1, 5, 1, 10).kind, RNF_PRACTICE_STEP);
  rnf_practice_loop_free(l2);
  CHECK_EQ(rnf_practice_history_index(0, 60), 0);
  CHECK_EQ(rnf_practice_history_index(0.999, 60), 59);
  CHECK_EQ(rnf_practice_history_index(0.5, 0), 0);
  CHECK_EQ(rnf_practice_history_index(7, 60), 59);
}

TEST_CASE("practice loop on the engine never records") {
  std::string tmp;
  rn_session* s = newSession(tmp);
  record(s, 30);
  REQUIRE_EQ(rn_practice_set_a(s, 0), RN_OK);
  record(s, 60, 30);
  REQUIRE_EQ(rn_practice_set_b(s, 0), RN_OK);
  auto slotLength = [&]() {
    rn_practice_slot_info i{};
    rn_practice_slot_get(s, 0, &i);
    return i.length_frames;
  };
  CHECK_EQ(slotLength(), uint64_t(60));
  const uint64_t takeLen = rn_take_length(s), frame = rn_frame(s), hash = rn_state_hash(s);
  const size_t takes = rn_take_count(s);
  REQUIRE_EQ(rn_practice_goto_a(s, 0), RN_OK);
  rn_step(s, RN_BTN_RIGHT, 0, 0, nullptr);
  const uint64_t videoAfterA = rn_video_hash(s);
  int stepsSinceA = 1, restarts = 0;
  std::vector<uint64_t> firstFrames, videoAtB;
  rnf_practice_loop* loop = rnf_practice_loop_new();
  rnf_frame_history* history = rnf_frame_history_new(60);
  double now = 0;
  int shownBack = 0;
  while (restarts < 3) {
    now += 1.0 / 60;
    rnf_practice_action a = rnf_practice_loop_tick(loop, now, rn_practice_frame(s), 1, slotLength());
    switch (a.kind) {
      case RNF_PRACTICE_STEP:
        rn_step(s, RN_BTN_RIGHT, 0, 0, nullptr);
        if (++stepsSinceA == 1) firstFrames.push_back(rn_video_hash(s));
        rnf_frame_history_append(history, rn_video(s));
        break;
      case RNF_PRACTICE_BEGIN_HOLD:
        videoAtB.push_back(rn_video_hash(s));
        CHECK_EQ(rn_practice_frame(s), uint64_t(60));
        break;
      case RNF_PRACTICE_HOLD: break;
      case RNF_PRACTICE_REWIND_FRAME: {
        int idx = rnf_practice_history_index(a.back, rnf_frame_history_count(history));
        CHECK(rnf_frame_history_frame(history, idx) != nullptr);
        shownBack = std::max(shownBack, idx);
        break;
      }
      case RNF_PRACTICE_RESTART:
        rn_practice_goto_a(s, 0);
        rnf_frame_history_clear(history);
        ++restarts;
        stepsSinceA = 0;
        CHECK_EQ(rn_practice_frame(s), uint64_t(0));
        break;
    }
    CHECK_EQ(rn_take_length(s), takeLen);
    CHECK_EQ(rn_frame(s), frame);
  }
  CHECK_EQ(rnf_practice_loop_loops(loop), 3);
  CHECK_EQ(firstFrames.size(), size_t(2));
  for (auto v : firstFrames) CHECK_EQ(v, videoAfterA);
  CHECK_EQ(std::set<uint64_t>(videoAtB.begin(), videoAtB.end()).size(), size_t(1));
  CHECK(shownBack > 20);
  REQUIRE_EQ(rn_set_mode(s, RN_MODE_RECORD), RN_OK);
  CHECK_EQ(rn_take_length(s), takeLen);
  CHECK_EQ(rn_frame(s), frame);
  CHECK_EQ(rn_take_count(s), takes);
  CHECK_EQ(rn_state_hash(s), hash);
  rnf_practice_loop_free(loop);
  rnf_frame_history_free(history);
  rn_session_close(s);
}

TEST_CASE("fast-forward stops at the take end without recording") {
  std::string tmp;
  rn_session* s = newSession(tmp);
  record(s, 100);
  rn_seek(s, 40);
  const uint64_t take = rn_active_take(s);
  const size_t takes = rn_take_count(s);
  rnf_fast_forward* ff = rnf_fast_forward_new();
  REQUIRE_EQ(rnf_fast_forward_begin(ff, s), RN_OK);
  CHECK(rnf_fast_forward_shows_recording(ff, s));
  CHECK_EQ(rn_get_mode(s), RN_MODE_REPLAY);
  int done = 0, end = 0, guardN = 0;
  while (!end && guardN++ < 100) REQUIRE_EQ(rnf_fast_forward_step(ff, s, 4, &done, &end), RN_OK);
  CHECK(end);
  CHECK_EQ(rn_frame(s), uint64_t(100));
  REQUIRE_EQ(rnf_fast_forward_step(ff, s, 4, &done, &end), RN_OK);
  CHECK_EQ(done, 0);
  CHECK(end);
  REQUIRE_EQ(rnf_fast_forward_end(ff, s), RN_OK);
  CHECK_FALSE(rnf_fast_forward_active(ff));
  CHECK_EQ(rn_get_mode(s), RN_MODE_RECORD);
  CHECK_EQ(rn_take_length(s), uint64_t(100));
  CHECK_EQ(rn_active_take(s), take);
  CHECK_EQ(rn_take_count(s), takes);
  rnf_fast_forward_free(ff);
  rn_session_close(s);
}

TEST_CASE("audio fade tail") {
  std::vector<int16_t> src(800, 12000), tail(3200);
  REQUIRE_EQ(rnf_audio_fade_tail(src.data(), src.size(), 4, tail.data(), tail.size()), size_t(3200));
  CHECK_EQ(tail.back(), 0);
  CHECK(tail.front() <= 12000);
  for (size_t i = 1; i < tail.size(); ++i) CHECK(tail[i] <= tail[i - 1]);
  CHECK_EQ(rnf_audio_fade_tail(src.data(), 0, 4, tail.data(), tail.size()), size_t(0));
}

TEST_CASE("frame history ring") {
  rnf_frame_history* h = rnf_frame_history_new(3);
  const size_t n = size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT;
  for (uint32_t v = 1; v <= 5; ++v) {
    std::vector<uint32_t> f(n, v);
    rnf_frame_history_append(h, f.data());
  }
  CHECK_EQ(rnf_frame_history_count(h), 3);
  CHECK_EQ(rnf_frame_history_frame(h, 0)[0], 5u);
  CHECK_EQ(rnf_frame_history_frame(h, 2)[100], 3u);
  CHECK(rnf_frame_history_frame(h, 3) == nullptr);
  rnf_frame_history_release(h);
  CHECK_EQ(rnf_frame_history_count(h), 0);
  rnf_frame_history_free(h);
}
