// Frontend core: VTR-style transport UX (docs/design/VTR_UX.md) - the overwrite condition, the
// retry undo window and the burst collapse (fake clock and a real session), the status label, the
// first-run coach lines and the Make Video dialog's range / quality presets.
#include <string>
#include <vector>

#include "support/FrontendTestUtil.h"
#include "support/rn_test.h"

using namespace rnft;

namespace {

struct Lang {
  std::string saved;
  explicit Lang(const char* lang) : saved(rnf_l10n_language()) { rnf_l10n_set_language(lang); }
  ~Lang() { rnf_l10n_set_language(saved.c_str()); }
};

rn_session* newSession(std::string& tmp) {
  tmp = rntest::tempDir("frontend-tape");
  std::string rom = rntest::writeTestRom(tmp);
  rn_session_options o;
  rn_session_options_init(&o);
#if !RN_PRODUCT_NES
  o.core = RN_CORE_MOCK;
#endif
  rn_session* s = nullptr;
  rn_session_new(rom.c_str(), nullptr, &o, &s);
  return s;
}

// Live play through the retry helper: n frames at 60 Hz from `t` (advances t).
void play(rnf_retry* r, rn_session* s, double& t, int n, uint8_t p1 = 0) {
  for (int i = 0; i < n; ++i) {
    rn_step_info info{};
    REQUIRE_EQ(rnf_retry_step(r, s, t, uint8_t(p1 ^ (i % 7 == 0 ? RN_BTN_A : 0)), 0, 0, &info), RN_OK);
    t += 1.0 / 60;
  }
}

std::vector<rn_take_info> takes(rn_session* s) {
  std::vector<rn_take_info> v(rn_take_count(s));
  for (size_t i = 0; i < v.size(); ++i) rn_take_get(s, i, &v[i]);
  return v;
}

}  // namespace

TEST_CASE("overwrite: RECORD before the end of the tape") {
  CHECK(rnf_tape_overwrites(RN_MODE_RECORD, 10, 20));
  CHECK_FALSE(rnf_tape_overwrites(RN_MODE_RECORD, 20, 20));
  CHECK_FALSE(rnf_tape_overwrites(RN_MODE_RECORD, 0, 0));
  CHECK_FALSE(rnf_tape_overwrites(RN_MODE_REPLAY, 10, 20));
  CHECK_FALSE(rnf_tape_overwrites(RN_MODE_PRACTICE, 10, 20));
}

TEST_CASE("retry: undo window, target and burst plans on a fake clock") {
  rnf_retry* r = rnf_retry_new();
  // Played take 1 to frame 900, rewound to 600 and retried at t = 100.
  rnf_retry_note_play(r, 99.0, 1, 900);
  rnf_retry_plan p = rnf_retry_begin(r, 100.0, 1, 0, 600, 900);
  CHECK_EQ(p.truncate, 0);
  CHECK_EQ(p.discard_take, 0u);
  CHECK_FALSE(rnf_retry_can_undo(r, 100.0, 1, RN_MODE_RECORD));  // not started yet
  rnf_retry_started(r, 100.0, 2);
  CHECK(rnf_retry_can_undo(r, 100.0, 2, RN_MODE_RECORD));
  CHECK(rnf_retry_can_undo(r, 109.9, 2, RN_MODE_RECORD));
  CHECK_FALSE(rnf_retry_can_undo(r, 110.0, 2, RN_MODE_RECORD));
  CHECK_FALSE(rnf_retry_can_undo(r, 105.0, 3, RN_MODE_RECORD));    // another take is active
  CHECK_FALSE(rnf_retry_can_undo(r, 105.0, 2, RN_MODE_PRACTICE));  // practicing
  CHECK(near(rnf_retry_undo_left(r, 104.0), 6.0, 1e-9));
  CHECK_EQ(rnf_retry_undo_left(r, 111.0), 0.0);
  // Played the attempt for 4 s, then a retry inside its own frames (branch frame 600): cut + continue.
  rnf_retry_note_play(r, 104.0, 2, 840);
  p = rnf_retry_begin(r, 107.0, 2, 600, 700, 840);
  CHECK_EQ(p.truncate, 1);
  CHECK_EQ(p.discard_take, 0u);
  rnf_retry_started(r, 107.0, 3);  // the cut copy
  // Then a retry before its branch frame: the attempt is dropped after the first step.
  rnf_retry_note_play(r, 108.0, 3, 760);
  p = rnf_retry_begin(r, 109.0, 3, 600, 500, 760);
  CHECK_EQ(p.truncate, 0);
  CHECK_EQ(p.discard_take, 3u);
  rnf_retry_started(r, 109.0, 4);
  // Undo: back to the continuation from before the burst (take 1 where it was played to), the
  // attempt goes. The window then closes.
  rnf_retry_undo_plan u{};
  CHECK_FALSE(rnf_retry_undo(r, 119.5, 4, RN_MODE_RECORD, &u));  // too late
  REQUIRE(rnf_retry_undo(r, 112.0, 4, RN_MODE_RECORD, &u));
  CHECK_EQ(u.take, 1u);
  CHECK_EQ(u.frame, 900u);
  CHECK_EQ(u.discard_take, 4u);
  CHECK_FALSE(rnf_retry_undo(r, 112.0, 4, RN_MODE_RECORD, &u));
  // An attempt played for 10 s or more is kept: a retry of it starts a new burst from it.
  rnf_retry_begin(r, 200.0, 1, 0, 300, 900);
  rnf_retry_started(r, 200.0, 5);
  rnf_retry_note_play(r, 212.0, 5, 1020);
  p = rnf_retry_begin(r, 215.0, 5, 300, 400, 1020);
  CHECK_EQ(p.truncate, 0);
  CHECK_EQ(p.discard_take, 0u);
  rnf_retry_started(r, 215.0, 6);
  REQUIRE(rnf_retry_undo(r, 216.0, 6, RN_MODE_RECORD, &u));
  CHECK_EQ(u.take, 5u);
  CHECK_EQ(u.frame, 1020u);
  // Never played on this take before the retry: the target is its end.
  rnf_retry_reset(r);
  rnf_retry_begin(r, 300.0, 7, 0, 10, 50);
  rnf_retry_started(r, 300.0, 8);
  REQUIRE(rnf_retry_undo(r, 301.0, 8, RN_MODE_RECORD, &u));
  CHECK_EQ(u.take, 7u);
  CHECK_EQ(u.frame, 50u);
  rnf_retry_free(r);
}

TEST_CASE("retry: rapid retries on a session keep one attempt; undo restores the tape") {
  std::string tmp;
  rn_session* s = newSession(tmp);
  REQUIRE(s);
  rnf_retry* r = rnf_retry_new();
  double t = 0;
  play(r, s, t, 600, RN_BTN_RIGHT);  // the tape: 600 frames
  const uint64_t original = rn_active_take(s);
  const uint64_t endHash = rn_state_hash(s);
  CHECK_FALSE(rnf_retry_can_undo(r, t, original, RN_MODE_RECORD));
  // Rewind to 300 and play on: a retry.
  REQUIRE_EQ(rn_seek(s, 300), RN_OK);
  t += 2;
  play(r, s, t, 120);
  const uint64_t attempt = rn_active_take(s);
  CHECK(attempt != original);
  CHECK_EQ(rn_take_count(s), 2u);
  CHECK(rnf_retry_can_undo(r, t, attempt, RN_MODE_RECORD));
  // Rewind a little (inside the attempt's own frames) and retry: the attempt is cut and continued.
  REQUIRE_EQ(rn_seek(s, 380), RN_OK);
  t += 1;
  play(r, s, t, 60);
  CHECK_EQ(rn_take_count(s), 2u);
  CHECK_EQ(rn_take_length(s), 440u);
  // Rewind before the attempt's start and retry: the attempt is dropped.
  REQUIRE_EQ(rn_seek(s, 250), RN_OK);
  t += 1;
  play(r, s, t, 30);
  CHECK_EQ(rn_take_count(s), 2u);
  for (auto& k : takes(s)) CHECK(k.id == original || k.is_active);
  // Undo: the original tape at its end; the attempt is gone.
  REQUIRE(rnf_retry_can_undo(r, t, rn_active_take(s), RN_MODE_RECORD));
  REQUIRE_EQ(rnf_retry_apply_undo(r, s, t), RN_OK);
  CHECK_EQ(rn_active_take(s), original);
  CHECK_EQ(rn_frame(s), 600u);
  CHECK_EQ(rn_take_count(s), 1u);
  CHECK_EQ(rn_state_hash(s), endHash);
  CHECK_EQ(rnf_retry_apply_undo(r, s, t), RN_ERR_NOT_FOUND);
  // A slow retry (played 10 s or more) is kept when retried.
  REQUIRE_EQ(rn_seek(s, 100), RN_OK);
  play(r, s, t, 660);  // 11 s
  const uint64_t keeper = rn_active_take(s);
  REQUIRE_EQ(rn_seek(s, 500), RN_OK);
  play(r, s, t, 30);
  CHECK_EQ(rn_take_count(s), 3u);
  CHECK(rn_active_take(s) != keeper);
  // Playback (REPLAY) is never an overwrite; practice plays nothing into the tape.
  REQUIRE_EQ(rn_seek(s, 200), RN_OK);
  REQUIRE_EQ(rn_set_mode(s, RN_MODE_REPLAY), RN_OK);
  const size_t before = rn_take_count(s);
  play(r, s, t, 30);
  CHECK_EQ(rn_take_count(s), before);
  CHECK_EQ(rn_frame(s), 230u);
  rnf_retry_free(r);
  rn_session_close(s);
}

TEST_CASE("retry: a bookmark on the attempt keeps it (the retry just branches)") {
  std::string tmp;
  rn_session* s = newSession(tmp);
  REQUIRE(s);
  rnf_retry* r = rnf_retry_new();
  double t = 0;
  play(r, s, t, 300);
  REQUIRE_EQ(rn_seek(s, 100), RN_OK);
  play(r, s, t, 60);
  uint64_t bm = 0;
  REQUIRE_EQ(rn_bookmark_add(s, "b", &bm), RN_OK);
  REQUIRE_EQ(rn_seek(s, 130), RN_OK);
  play(r, s, t, 10);
  CHECK_EQ(rn_take_count(s), 3u);
  rnf_retry_free(r);
  rn_session_close(s);
}

TEST_CASE("status label: only non-default states") {
  Lang en("en");
  rnf_tape_status st{};
  st.mode = RN_MODE_RECORD;
  st.slow_rate = 1;
  CHECK_EQ(take(rnf_tape_status_label(&st)), "");
  st.rewinding = 1;
  CHECK_EQ(take(rnf_tape_status_label(&st)), "◀◀ Rewind");
  st.rewinding = 0;
  st.fast_forward = 1;
  st.slow_rate = 2;
  CHECK_EQ(take(rnf_tape_status_label(&st)), "▶▶ Fast-forward · Slow ½");
  st.fast_forward = 0;
  st.slow_rate = 1;
  st.mode = RN_MODE_REPLAY;
  st.end_of_tape = 1;
  CHECK_EQ(take(rnf_tape_status_label(&st)), "End of tape · Playback");
  st.end_of_tape = 0;
  st.mode = RN_MODE_PRACTICE;
  CHECK_EQ(take(rnf_tape_status_label(&st)), "Practice (not recorded)");
  Lang ja("ja");
  CHECK_EQ(take(rnf_tape_status_label(&st)), "練習中（記録しません）");
  st.mode = RN_MODE_REPLAY;
  CHECK_EQ(take(rnf_tape_status_label(&st)), "再生中");
  CHECK_EQ(take(rnf_tape_time_label(0)), "0:00");
  CHECK_EQ(take(rnf_tape_time_label(uint64_t(902.5 / rnf_frame_period()))), "15:02");
  CHECK_EQ(take(rnf_tape_time_label(uint64_t(3658.5 / rnf_frame_period()))), "1:00:58");
}

TEST_CASE("coach: three tips, each once, spaced out") {
  Lang en("en");
  rnf_coach* c = rnf_coach_new(0);
  rnf_coach_input in{};
  in.playing = 1;
  CHECK_EQ(rnf_coach_tick(c, 10.0, &in), RNF_COACH_NONE);
  CHECK_EQ(rnf_coach_tick(c, 14.9, &in), RNF_COACH_NONE);
  CHECK_EQ(rnf_coach_tick(c, 15.0, &in), RNF_COACH_REWIND);
  CHECK_EQ(rnf_coach_shown(c), uint32_t(RNF_COACH_REWIND));
  // A rewind stops right away: the tip waits for the gap.
  in.playing = 0;
  in.rewind_stopped = 1;
  CHECK_EQ(rnf_coach_tick(c, 16.0, &in), RNF_COACH_NONE);
  in.rewind_stopped = 0;
  CHECK_EQ(rnf_coach_tick(c, 20.9, &in), RNF_COACH_NONE);
  CHECK_EQ(rnf_coach_tick(c, 21.0, &in), RNF_COACH_RETRY);
  // End of tape: only with a minute recorded.
  in.paused_at_end = 1;
  in.take_length = RNF_COACH_VIDEO_MIN_FRAMES - 1;
  CHECK_EQ(rnf_coach_tick(c, 40.0, &in), RNF_COACH_NONE);
  in.take_length = RNF_COACH_VIDEO_MIN_FRAMES;
  CHECK_EQ(rnf_coach_tick(c, 41.0, &in), RNF_COACH_VIDEO);
  CHECK_EQ(rnf_coach_shown(c), uint32_t(RNF_COACH_REWIND | RNF_COACH_RETRY | RNF_COACH_VIDEO));
  in.rewind_stopped = 1;
  CHECK_EQ(rnf_coach_tick(c, 100.0, &in), RNF_COACH_NONE);  // never again
  rnf_coach_free(c);
  // Shown before (settings): nothing.
  c = rnf_coach_new(RNF_COACH_REWIND | RNF_COACH_RETRY | RNF_COACH_VIDEO);
  in = rnf_coach_input{};
  in.playing = 1;
  in.rewind_stopped = 1;
  CHECK_EQ(rnf_coach_tick(c, 0.0, &in), RNF_COACH_NONE);
  CHECK_EQ(rnf_coach_tick(c, 60.0, &in), RNF_COACH_NONE);
  rnf_coach_free(c);
  CHECK_EQ(take(rnf_coach_text(RNF_COACH_REWIND, "Delete")), "Hold Delete to rewind");
  CHECK_EQ(take(rnf_coach_text(RNF_COACH_REWIND, nullptr)), "Hold L2 to rewind");
  Lang ja("ja");
  CHECK_EQ(take(rnf_coach_text(RNF_COACH_REWIND, "L2")), "L2 を押し続けると巻き戻せます");
  CHECK_EQ(take(rnf_coach_text(RNF_COACH_RETRY, nullptr)), "ここからプレイするとやり直せます");
  CHECK_EQ(take(rnf_coach_text(RNF_COACH_VIDEO, nullptr)), "☰ › 共有 › 動画にする で動画にできます");
}

TEST_CASE("make video: ranges, quality presets, summary") {
  uint64_t a = 1, b = 1;
  CHECK(rnf_video_range(RNF_VIDEO_RANGE_ALL, 300, 900, nullptr, &a, &b));
  CHECK_EQ(a, 0u);
  CHECK_EQ(b, 900u);
  CHECK(rnf_video_range(RNF_VIDEO_RANGE_TO_HERE, 300, 900, nullptr, &a, &b));
  CHECK_EQ(b, 300u);
  CHECK_FALSE(rnf_video_range(RNF_VIDEO_RANGE_TO_HERE, 0, 900, nullptr, &a, &b));
  CHECK_FALSE(rnf_video_range(RNF_VIDEO_RANGE_ALL, 0, 0, nullptr, &a, &b));
  rnf_timeline_range sec{2, 120, 1, 480};
  CHECK(rnf_video_range(RNF_VIDEO_RANGE_SECTION, 0, 900, &sec, &a, &b));
  CHECK_EQ(a, 120u);
  CHECK_EQ(b, 480u);
  sec.has_b = 0;
  CHECK_FALSE(rnf_video_range(RNF_VIDEO_RANGE_SECTION, 0, 900, &sec, &a, &b));
  CHECK_FALSE(rnf_video_range(RNF_VIDEO_RANGE_SECTION, 0, 900, nullptr, &a, &b));
  CHECK_EQ(rnf_video_quality(0), RNF_QUALITY_STANDARD);
  CHECK_EQ(rnf_video_quality(1), RNF_QUALITY_HIGH);
  CHECK_EQ(rnf_video_quality(2), RNF_QUALITY_LIGHT);
  for (int i = 0; i < RNF_VIDEO_QUALITY_PRESETS; ++i) CHECK_EQ(rnf_video_quality_preset(rnf_video_quality(i)), i);
  Lang ja("ja");
  CHECK_EQ(std::string(rnf_menu_text(rnf_video_quality_label(2))), "小さく");
  CHECK_EQ(take(rnf_video_summary(uint64_t(902.5 / rnf_frame_period()), 90400000)), "15:02 ・ 目安 91 MB");
  Lang en("en");
  CHECK_EQ(take(rnf_video_summary(600, 1500000000)), "0:09 · approx. 1.5 GB");
  CHECK_EQ(std::string(rnf_menu_text(rnf_video_quality_label(1))), "High");
}

TEST_CASE("tape time follows the content frame rate") {
  Lang en("en");
  CHECK_EQ(take(rnf_video_summary_for_period(5963, 1500000000, 100.0 / 5963)), "1:40 · approx. 1.5 GB");
  CHECK_EQ(take(rnf_video_summary_for_period(600, 1500000000, 0)), take(rnf_video_summary(600, 1500000000)));
  CHECK_EQ(take(rnf_tape_time_label_for_period(5963, 100.0 / 5963)), "1:40");
  CHECK_EQ(take(rnf_tape_time_label_for_period(3600, 1.0 / 30)), "2:00");
  CHECK_EQ(take(rnf_tape_time_label_for_period(3600, 0)), take(rnf_tape_time_label(3600)));
}
