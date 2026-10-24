// VTR-style transport UX (docs/design/VTR_UX.md): the overwrite condition, the retry undo window and
// the collapse of rapid retries, the status label, the first-run coach lines and the Make Video
// dialog's range / quality presets. Shared by every frontend; they only draw.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "common.hpp"

using namespace rnf;

// ------------------------------------------------------------------ overwrite

int rnf_tape_overwrites(rn_mode mode, uint64_t frame, uint64_t take_length) {
  return mode == RN_MODE_RECORD && frame < take_length ? 1 : 0;
}

// ------------------------------------------------------------------ retry undo + burst collapse

struct rnf_retry {
  // Where the tape was last played to (rnf_retry_note_play).
  bool hasPlayed = false;
  uint64_t playedTake = 0, playedFrame = 0;
  double playedAt = 0;
  // The current attempt (the take an overwrite started) and the undo target.
  uint64_t attempt = 0;
  double attemptStart = 0, attemptLastPlayed = 0;
  bool undoOpen = false;  // closed by undo / reset
  uint64_t targetTake = 0, targetFrame = 0;
};

rnf_retry* rnf_retry_new(void) {
  try { return new rnf_retry; } catch (...) { return nullptr; }
}
void rnf_retry_free(rnf_retry* r) { delete r; }
void rnf_retry_reset(rnf_retry* r) {
  if (r) *r = rnf_retry();
}

void rnf_retry_note_play(rnf_retry* r, double now, uint64_t take_id, uint64_t frame) {
  if (!r) return;
  r->hasPlayed = true;
  r->playedTake = take_id;
  r->playedFrame = frame;
  r->playedAt = now;
  if (r->attempt != 0 && take_id == r->attempt) r->attemptLastPlayed = now;
}

rnf_retry_plan rnf_retry_begin(rnf_retry* r, double now, uint64_t active_take, uint64_t branch_frame, uint64_t frame,
                               uint64_t take_length) {
  rnf_retry_plan plan{};
  if (!r) return plan;
  // A burst: the attempt being retried was left (last played) within the window of its start.
  const bool burst = r->attempt != 0 && active_take == r->attempt && r->targetTake != 0 &&
                     r->attemptLastPlayed - r->attemptStart < RNF_RETRY_UNDO_SECONDS;
  if (burst) {
    // Inside the attempt's own frames: cut it at the cursor and continue it; at / before its branch
    // frame the retry branches off the earlier take and the attempt is dropped.
    if (frame > branch_frame && frame < take_length) plan.truncate = 1;
    else plan.discard_take = r->attempt;
  } else {
    // The continuation from before this retry: where the tape was last played to on this take,
    // else its end.
    r->targetTake = active_take;
    r->targetFrame = r->hasPlayed && r->playedTake == active_take ? r->playedFrame : take_length;
  }
  r->attempt = 0;  // until rnf_retry_started names the new one
  (void)now;
  return plan;
}

void rnf_retry_started(rnf_retry* r, double now, uint64_t attempt_take) {
  if (!r) return;
  r->attempt = attempt_take;
  r->attemptStart = r->attemptLastPlayed = now;
  r->undoOpen = attempt_take != 0 && r->targetTake != 0 && attempt_take != r->targetTake;
}

static bool undoAvailable(const rnf_retry* r, double now, uint64_t active_take, rn_mode mode) {
  return r && r->undoOpen && r->attempt != 0 && active_take == r->attempt && mode != RN_MODE_PRACTICE &&
         now - r->attemptStart < RNF_RETRY_UNDO_SECONDS && now >= r->attemptStart;
}

int rnf_retry_can_undo(const rnf_retry* r, double now, uint64_t active_take, rn_mode mode) {
  return undoAvailable(r, now, active_take, mode) ? 1 : 0;
}

double rnf_retry_undo_left(const rnf_retry* r, double now) {
  if (!r || !r->undoOpen || r->attempt == 0) return 0;
  double left = RNF_RETRY_UNDO_SECONDS - (now - r->attemptStart);
  return left > 0 ? left : 0;
}

int rnf_retry_undo(rnf_retry* r, double now, uint64_t active_take, rn_mode mode, rnf_retry_undo_plan* out) {
  if (!undoAvailable(r, now, active_take, mode)) return 0;
  rnf_retry_undo_plan p{r->targetTake, r->targetFrame, r->attempt};
  if (out) *out = p;
  // Back on the continuation: the next retry starts a new burst from there.
  r->undoOpen = false;
  r->attempt = 0;
  r->hasPlayed = true;
  r->playedTake = r->targetTake;
  r->playedFrame = r->targetFrame;
  r->targetTake = 0;
  return 1;
}

static rn_status retryStep(rnf_retry* r, rn_session* s, double now, uint8_t p1, uint8_t p2, uint8_t events,
                           const rn_input_record* record, rn_step_info* info) {
  if (!s) return fail(RN_ERR_INVALID_ARG, "null session");
  const rn_mode mode = rn_get_mode(s);
  const bool overwrite = r && rnf_tape_overwrites(mode, rn_frame(s), rn_take_length(s));
  rnf_retry_plan plan{};
  if (overwrite) {
    const uint64_t head = rn_active_take(s);
    uint64_t branch = 0;
    for (size_t i = 0, n = rn_take_count(s); i < n; ++i) {
      rn_take_info t{};
      if (rn_take_get(s, i, &t) == RN_OK && t.id == head) {
        branch = t.branch_frame;
        break;
      }
    }
    plan = rnf_retry_begin(r, now, head, branch, rn_frame(s), rn_take_length(s));
    // A refused cut (a bookmark / A/B slot on the attempt) just branches as before.
    if (plan.truncate) (void)rn_take_truncate(s);
  }
  rn_step_info local{};
  rn_step_info* si = info ? info : &local;
  rn_status st = record ? rn_step_record(s, record, si) : rn_step(s, p1, p2, events, si);
  if (st != RN_OK) return st;
  if (overwrite) {
    if (plan.discard_take != 0 && plan.discard_take != rn_active_take(s)) (void)rn_take_discard(s, plan.discard_take);
    rnf_retry_started(r, now, rn_active_take(s));
  }
  if (r && (mode == RN_MODE_RECORD || mode == RN_MODE_REPLAY)) rnf_retry_note_play(r, now, si->take_id, si->frame);
  return RN_OK;
}

rn_status rnf_retry_step(rnf_retry* r, rn_session* s, double now, uint8_t p1, uint8_t p2, uint8_t events,
                         rn_step_info* info) {
  return retryStep(r, s, now, p1, p2, events, nullptr, info);
}

rn_status rnf_retry_step_record(rnf_retry* r, rn_session* s, double now, const rn_input_record* record,
                                rn_step_info* info) {
  if (!record) return fail(RN_ERR_INVALID_ARG, "null input record");
  return retryStep(r, s, now, 0, 0, 0, record, info);
}

rn_status rnf_retry_apply_undo(rnf_retry* r, rn_session* s, double now) {
  if (!s) return fail(RN_ERR_INVALID_ARG, "null session");
  rnf_retry_undo_plan p{};
  if (!rnf_retry_undo(r, now, rn_active_take(s), rn_get_mode(s), &p)) return fail(RN_ERR_NOT_FOUND, "nothing to undo");
  rn_status st = rn_take_activate(s, p.take);
  if (st != RN_OK) return st;
  const uint64_t len = rn_take_length(s);
  st = rn_seek(s, p.frame < len ? p.frame : len);
  if (st != RN_OK) return st;
  if (p.discard_take != 0 && p.discard_take != rn_active_take(s)) (void)rn_take_discard(s, p.discard_take);
  return RN_OK;
}

// ------------------------------------------------------------------ status label

char* rnf_tape_status_label(const rnf_tape_status* st) {
  if (!st) return dup("");
  std::vector<std::string> parts;
  if (st->rewinding) parts.push_back(tr(RNF_L("◀◀ Rewind")));
  else if (st->fast_forward) parts.push_back(tr(RNF_L("▶▶ Fast-forward")));
  else if (st->end_of_tape) parts.push_back(tr(RNF_L("End of tape")));
  if (st->slow_rate == 2) parts.push_back(tr(RNF_L("Slow ½")));
  else if (st->slow_rate >= 4) parts.push_back(tr(RNF_L("Slow ¼")));
  if (st->mode == RN_MODE_REPLAY) parts.push_back(tr(RNF_L("Playback (status)")));
  else if (st->mode == RN_MODE_PRACTICE) parts.push_back(tr(RNF_L("Practice (not recorded)")));
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += " · ";
    out += parts[i];
  }
  return dup(out);
}

char* rnf_tape_time_label(uint64_t frames) {
  return rnf_tape_time_label_for_period(frames, rnf_frame_period());
}
char* rnf_tape_time_label_for_period(uint64_t frames, double frame_period) {
  if (!(frame_period > 0) || !std::isfinite(frame_period)) frame_period = rnf_frame_period();
  const uint64_t secs = uint64_t(std::floor(double(frames) * frame_period + 1e-9));
  char buf[32];
  if (secs >= 3600)
    std::snprintf(buf, sizeof buf, "%llu:%02llu:%02llu", (unsigned long long)(secs / 3600),
                  (unsigned long long)(secs / 60 % 60), (unsigned long long)(secs % 60));
  else
    std::snprintf(buf, sizeof buf, "%llu:%02llu", (unsigned long long)(secs / 60), (unsigned long long)(secs % 60));
  return dup(buf);
}

// ------------------------------------------------------------------ first-run coach

struct rnf_coach {
  uint32_t shown = 0;
  uint32_t pending = 0;
  bool started = false;
  double firstPlay = 0;
  bool anyShown = false;
  double lastShown = 0;
};

rnf_coach* rnf_coach_new(uint32_t shown) {
  try {
    rnf_coach* c = new rnf_coach;
    c->shown = shown & (RNF_COACH_REWIND | RNF_COACH_RETRY | RNF_COACH_VIDEO);
    return c;
  } catch (...) {
    return nullptr;
  }
}
void rnf_coach_free(rnf_coach* c) { delete c; }
uint32_t rnf_coach_shown(const rnf_coach* c) { return c ? c->shown : 0; }

rnf_coach_tip rnf_coach_tick(rnf_coach* c, double now, const rnf_coach_input* in) {
  if (!c || !in) return RNF_COACH_NONE;
  if (in->playing && !c->started) {
    c->started = true;
    c->firstPlay = now;
  }
  if (c->started && in->playing && now - c->firstPlay >= RNF_COACH_REWIND_DELAY_SECONDS) c->pending |= RNF_COACH_REWIND;
  if (in->rewind_stopped) c->pending |= RNF_COACH_RETRY;
  if (in->paused_at_end && in->take_length >= RNF_COACH_VIDEO_MIN_FRAMES) c->pending |= RNF_COACH_VIDEO;
  c->pending &= ~c->shown;
  if (!c->pending) return RNF_COACH_NONE;
  if (c->anyShown && now - c->lastShown < RNF_COACH_GAP_SECONDS) return RNF_COACH_NONE;
  for (uint32_t bit : {uint32_t(RNF_COACH_REWIND), uint32_t(RNF_COACH_RETRY), uint32_t(RNF_COACH_VIDEO)}) {
    if (!(c->pending & bit)) continue;
    // The rewind tip waits for play (it talks about what to do while playing).
    if (bit == RNF_COACH_REWIND && !in->playing) continue;
    c->pending &= ~bit;
    c->shown |= bit;
    c->anyShown = true;
    c->lastShown = now;
    return rnf_coach_tip(bit);
  }
  return RNF_COACH_NONE;
}

char* rnf_coach_text(rnf_coach_tip tip, const char* rewind_control) {
  switch (tip) {
    case RNF_COACH_REWIND:
      return dup(trf(RNF_L("Hold %@ to rewind"), {argS(rewind_control && *rewind_control ? rewind_control : "L2")}));
    case RNF_COACH_RETRY: return dup(tr(RNF_L("Play from here to retry")));
    case RNF_COACH_VIDEO: return dup(tr(RNF_L("☰ › Share › Make Video turns this into a video")));
    default: return dup("");
  }
}

// ------------------------------------------------------------------ Make Video dialog

int rnf_video_range(rnf_video_range_kind kind, uint64_t cursor, uint64_t take_length,
                    const rnf_timeline_range* section, uint64_t* start, uint64_t* end) {
  uint64_t a = 0, b = 0;
  switch (kind) {
    case RNF_VIDEO_RANGE_ALL: b = take_length; break;
    case RNF_VIDEO_RANGE_TO_HERE: b = cursor < take_length ? cursor : take_length; break;
    case RNF_VIDEO_RANGE_SECTION:
      if (section && section->has_b && section->a < section->b && section->b <= take_length) {
        a = section->a;
        b = section->b;
      }
      break;
  }
  if (b <= a) return 0;
  if (start) *start = a;
  if (end) *end = b;
  return 1;
}

rnf_export_quality rnf_video_quality(int preset) {
  switch (preset) {
    case 1: return RNF_QUALITY_HIGH;
    case 2: return RNF_QUALITY_LIGHT;
    default: return RNF_QUALITY_STANDARD;
  }
}

int rnf_video_quality_preset(rnf_export_quality quality) {
  switch (quality) {
    case RNF_QUALITY_HIGH: return 1;
    case RNF_QUALITY_LIGHT: return 2;
    default: return 0;
  }
}

const char* rnf_video_quality_label(int preset) {
  switch (preset) {
    case 1: return RNF_L("High (video quality)");
    case 2: return RNF_L("Small (video quality)");
    default: return RNF_L("Standard (YouTube)");
  }
}

char* rnf_video_summary(uint64_t frames, int64_t bytes) {
  return rnf_video_summary_for_period(frames, bytes, rnf_frame_period());
}
char* rnf_video_summary_for_period(uint64_t frames, int64_t bytes, double frame_period) {
  char* t = rnf_tape_time_label_for_period(frames, frame_period);
  std::string len = t ? t : "";
  rnf_string_free(t);
  char size[32];
  if (bytes >= 1000000000) std::snprintf(size, sizeof size, "%.1f GB", double(bytes) / 1e9);
  else std::snprintf(size, sizeof size, "%.0f MB", std::ceil(double(bytes > 0 ? bytes : 0) / 1e6));
  return dup(trf(RNF_L("%@ · approx. %@"), {argS(len), argS(size)}));
}
