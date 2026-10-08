// SPDX-License-Identifier: GPL-2.0-or-later
#include "emulation.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "host_clock.h"
#include "l10n.h"

namespace rnl {

namespace {
std::string lastError() { return rn_last_error(); }
}  // namespace

std::string SlotInfo::displayName() const { return name.empty() ? TRF("Section %lld", {index + 1}) : name; }

bool SessionStructure::hasContent() const {
  for (const TakeInfo& t : takes)
    if (t.length > 0) return true;
  if (!bookmarks.empty()) return true;
  for (const SlotInfo& s : slots)
    if (s.hasA) return true;
  return false;
}

const char* EmulationController::practiceBlockedText() {
  return TR("Not available while practicing. Use “Stop Practicing” to return to the take first");
}

EmulationController::EmulationController(rn_input* input, AudioSink* audio) : input_(input), audio_(audio) {
  flash_ = rn_flash_filter_new(flashLevel_);
  ff_ = rnf_fast_forward_new();
  stepRepeater_ = rnf_step_repeater_new();
  practiceLoop_ = rnf_practice_loop_new();
  history_ = rnf_frame_history_new(60);
  display_.assign(size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT, 0xFF000000u);
}

EmulationController::~EmulationController() {
  closeSession();
  rn_flash_filter_free(flash_);
  rnf_fast_forward_free(ff_);
  rnf_step_repeater_free(stepRepeater_);
  rnf_practice_loop_free(practiceLoop_);
  rnf_frame_history_free(history_);
}

void EmulationController::notice(const std::string& s) {
  if (onNotice) onNotice(s);
}

void EmulationController::reportError(const std::string& title) {
  std::string msg = lastError();
  if (onError) onError(title, msg);
  else std::fprintf(stderr, "%s: %s\n", title.c_str(), msg.c_str());
}

void EmulationController::setMuted(bool m) {
  if (audio_) audio_->setMuted(m);
}

void EmulationController::setPaused(bool p) {
  bool old = paused_;
  paused_ = p;
  if (p == old) return;
  if (!p) {
    pauseHintShown_ = false;
    rnf_step_repeater_reset(stepRepeater_);
  } else {
    autosaveSoon_ = true;  // pausing persists right away (resume)
  }
  if (onPausedChanged) onPausedChanged(p);
}

// ------------------------------------------------------------------ session lifecycle

void EmulationController::install(rn_session* s) {
  if (s != session_) closeSession();
  session_ = s;
  bool fresh = s && rn_take_length(s) == 0 && rn_frame(s) == 0 && rn_get_mode(s) == RN_MODE_RECORD;
  setPaused(!fresh);
  slow_ = 1;
  advanceRemaining_ = 0;
  pendingEvents_ = 0;
  endOfTake_ = false;
  uiRewindHeld_ = uiFastForwardHeld_ = false;
  rewinding_ = fastForward_ = false;
  rewindTicks_ = 0;
  lastPressSeq_ = pressSequence ? pressSequence() : 0;
  pauseHintShown_ = false;
  setMuted(true);
  lastAutosave_ = nowSeconds();
  lastFullSave_ = 0;
  autosaveFailed_ = false;
  autosaveSoon_ = false;
  structureDirty_ = true;
  lastStructureTake_ = ~uint64_t(0);
  resetFlashFilter();
  rnf_fast_forward_free(ff_);
  ff_ = rnf_fast_forward_new();
  ffBlocked_ = false;
  rnf_step_repeater_reset(stepRepeater_);
  practiceSlot_ = -1;
  practiceLength_ = 0;
  rnf_practice_loop_reset(practiceLoop_);
  rnf_frame_history_release(history_);
  pendingThumb_ = false;
  if (s && rn_get_mode(s) == RN_MODE_PRACTICE) rn_set_mode(s, RN_MODE_RECORD);  // never persisted; defensive
  if (observer) observer->sessionInstalled(s);
  if (s) {
    publishVideo();
    if (observer) observer->frameShown(s, rn_frame(s));
  } else {
    std::fill(display_.begin(), display_.end(), 0xFF000000u);
    pictureDirty_ = true;
  }
  updateStatus();
}

void EmulationController::closeSession() {
  if (!session_) return;
  rnf_fast_forward_end(ff_, session_);
  rn_session_close(session_);
  session_ = nullptr;
  rn_input_release_all(input_);
  setMuted(true);
  if (observer) observer->sessionInstalled(nullptr);
  updateStatus();
}

// ------------------------------------------------------------------ picture / flash filter

void EmulationController::setFlashLevel(rn_flash_level l) {
  if (l == flashLevel_) return;
  flashLevel_ = l;
  rn_flash_filter_set_level(flash_, l);
  flashAltered_ = false;
  if (l == RN_FLASH_OFF) publishVideo();  // the unfiltered picture right away
}

void EmulationController::resetFlashFilter() {
  rn_flash_filter_reset(flash_);
  flashAltered_ = false;
}

void EmulationController::show(const uint32_t* v) {
  signal_ = Signal();
  rn_video_indices_info vi{};
  if (session_ && v == rn_video(session_) && rn_video_indices(session_, &vi) == RN_OK && vi.codes) {
    signal_.codes = vi.codes;
    signal_.burstPhase = vi.burst_phase;
    signal_.ordinal = vi.frame;
  } else {
    signal_.ordinal = ++pictureOrdinal_;
  }
  if (flashLevel_ == RN_FLASH_OFF) {
    std::memcpy(display_.data(), v, display_.size() * 4);
    flashAltered_ = false;
  } else {
    rn_flash_info info{};
    rn_flash_filter_process(flash_, v, display_.data(), &info);
    flashAltered_ = info.altered != 0;
    if (flashAltered_) lastFlashTick_ = tickCount_;
  }
  signal_.flashAltered = flashAltered_;
  pictureDirty_ = true;
}

void EmulationController::publishVideo(bool continuous) {
  if (!session_) return;
  const uint32_t* v = rn_video(session_);
  if (!v) return;
  if (!continuous) resetFlashFilter();
  show(v);
}

// ------------------------------------------------------------------ seek / step

void EmulationController::seekCommand(uint64_t f) {
  if (!session_) return;
  if (rn_get_mode(session_) == RN_MODE_PRACTICE) {
    // The take cursor is frozen while practicing: "Go to Start" means back to A.
    if (f == 0 && practiceSlot_ >= 0) startPractice(practiceSlot_);
    else notice(practiceBlockedText());
    return;
  }
  if (rnf_fast_forward_active(ff_)) endFastForward();
  if (rn_seek(session_, std::min(f, rn_take_length(session_))) == RN_OK) endOfTake_ = false;
  else reportError(TR("Couldn’t move"));
  setPaused(true);
  setMuted(true);
  publishVideo();
  pendingThumb_ = true;
  pendingThumbFrame_ = rn_frame(session_);
}

void EmulationController::seek(uint64_t f) { seekCommand(f); }

void EmulationController::scrub(uint64_t f) {
  if (!session_ || rn_get_mode(session_) == RN_MODE_PRACTICE) return;
  setPaused(true);
  seekCommand(f);
}

void EmulationController::jumpSeconds(double seconds) {
  if (!session_) return;
  uint64_t n = uint64_t(std::abs(seconds) * 60);
  if (seconds < 0) stepBack(n);
  else seekCommand(std::min(rn_take_length(session_), rn_frame(session_) + n));
}

void EmulationController::stepFrame(int dir) {
  if (!session_) return;
  setPaused(true);
  if (rn_get_mode(session_) == RN_MODE_PRACTICE) rnf_practice_loop_interrupt(practiceLoop_);
  if (dir > 0) {
    advanceRemaining_ += 1;
    return;
  }
  stepBack(1);
}

void EmulationController::frameAdvance(int n) {
  if (!session_) return;
  setPaused(true);
  advanceRemaining_ += std::max(0, n);
}

void EmulationController::stepBack(uint64_t n) {
  if (!session_) return;
  setPaused(true);
  advanceRemaining_ = 0;
  if (rn_get_mode(session_) == RN_MODE_PRACTICE) {
    rnf_practice_loop_interrupt(practiceLoop_);
    if (rn_rewind(session_, n) == RN_OK) {
      setMuted(true);
      publishVideo();
    } else {
      reportError(TR("Couldn’t go back"));
    }
    return;
  }
  uint64_t f = rn_frame(session_);
  seekCommand(f >= n ? f - n : 0);
}

void EmulationController::pausedStep(int dir, bool down) {
  if (!session_) return;
  if (down) {
    if (!paused_) return;
    stepFrame(rnf_step_repeater_press(stepRepeater_, dir));
  } else {
    rnf_step_repeater_release(stepRepeater_, dir);
  }
}

void EmulationController::togglePause() {
  advanceRemaining_ = 0;
  if (!session_) {
    setPaused(!paused_);
    return;
  }
  rn_mode m = rn_get_mode(session_);
  if (paused_ && rnf_record_toggle_restarts_on_play(m == RN_MODE_RECORD, m == RN_MODE_PRACTICE, rn_frame(session_),
                                                    rn_take_length(session_)))
    seekCommand(0);  // replay at the take end: play from the beginning
  setPaused(!paused_);
}

void EmulationController::toggleSlow() {
  slow_ = rnf_slow_toggled(slow_);
  notice(slow_ == 1 ? TR("Normal Speed") : TR("Slow 1/2"));
}

// ------------------------------------------------------------------ the frame

EmulationController::Tick EmulationController::tick() {
  Tick t;
  tickCount_ += 1;
  if (!session_) {
    t.newPicture = pictureDirty_;
    pictureDirty_ = false;
    return t;
  }
  rn_session* s = session_;
  // Read the press counter before polling hotkeys so a hotkey press is never mistaken for a
  // game-button press below.
  uint64_t pressSeq = pressSequence ? pressSequence() : 0;
  uint32_t edges = 0, held = 0;
  rn_input_poll_hotkeys(input_, &edges, &held);
  bool wasPaused = paused_;
  if (edges) handleHotkeys(edges);
  if (pressSeq != lastPressSeq_) {
    lastPressSeq_ = pressSeq;
    // Game input is not emulated while paused; say so once instead of silently ignoring it.
    if (wasPaused && paused_ && edges == 0 && held == 0 && advanceRemaining_ == 0 && !pauseHintShown_ &&
        !uiRewindHeld_) {
      pauseHintShown_ = true;
      notice(TR("Paused. Press Space, or the back button on the controller, to resume"));
    }
  }

  bool practicing = rn_get_mode(s) == RN_MODE_PRACTICE;
  bool wantRewind = uiRewindHeld_ || (held & RN_HK_REWIND) != 0;
  bool wantFF = !wantRewind && !practicing && (uiFastForwardHeld_ || (held & RN_HK_FAST_FORWARD) != 0);
  if (wantFF && !fastForward_) beginFastForward();
  if (!wantFF && fastForward_) endFastForward();
  fastForward_ = wantFF;

  bool emulatedLive = false;
  if (wantRewind) {
    if (!rewinding_) resetFlashFilter();  // rewind start: a new continuous (backwards) sequence
    rewinding_ = true;
    if (practicing) rnf_practice_loop_interrupt(practiceLoop_);
    setMuted(true);
    rewindTicks_ += 1;
    uint64_t n = uint64_t(rnf_hold_speed(rewindTicks_));  // the same curve as fast-forward
    // Practice: rewinds the practice run only (the engine stops at A); take: clamps at 0.
    bool can = false;
    if (practicing) {
      rn_practice_status ps{};
      rn_practice_get_status(s, &ps);
      can = ps.rewind_available > 0;
    } else {
      can = rn_frame(s) > 0;
    }
    if (can) {
      if (rn_rewind(s, n) == RN_OK) {
        endOfTake_ = false;
        publishVideo(true);
      } else {
        reportError(TR("Rewind failed"));
        uiRewindHeld_ = false;
      }
    }
  } else {
    if (rewinding_) {
      rewinding_ = false;
      rewindTicks_ = 0;
      if (pauseAfterRewind) setPaused(true);
    }
    if (paused_) {
      int d = rnf_step_repeater_tick(stepRepeater_);
      if (d != 0) stepFrame(d);
    }
    if (fastForward_) {
      tickFastForward();
    } else if (practicing && !paused_) {
      uint64_t before = emulatedFrames_;
      tickPractice();
      emulatedLive = emulatedFrames_ != before;
    } else {
      int steps = 0;
      bool audible = false;
      if (paused_) {
        // Frame-advance users press buttons while paused on purpose: no hint for them.
        if (advanceRemaining_ > 0) {
          steps = 1;
          advanceRemaining_ -= 1;
          pauseHintShown_ = true;
        }
      } else {
        if (tickCount_ % uint64_t(slow_) == 0) steps = 1;
        audible = slow_ == 1;
      }
      setMuted(!audible);
      int stepped = 0;
      for (int i = 0; i < steps; ++i) {
        if (!stepOnce(audible)) break;
        stepped += 1;
      }
      emulatedLive = stepped > 0 && rn_get_mode(s) != RN_MODE_REPLAY;
      // A frame held back by the flash filter while nothing new is emulated (pause, slow
      // motion) is re-filtered every tick, so the picture settles on the real frame.
      if (stepped == 0 && flashAltered_ && !rewinding_) publishVideo(true);
    }
  }
  bool active = flashLevel_ != RN_FLASH_OFF && lastFlashTick_ != 0 && tickCount_ - lastFlashTick_ < 45;
  flashActiveShown_ = active;
  updateStatus();
  t.emulated = emulatedLive;
  t.newPicture = pictureDirty_;
  pictureDirty_ = false;
  return t;
}

bool EmulationController::stepOnce(bool audible) {
  rn_session* s = session_;
  rn_mode mode = rn_get_mode(s);
  bool live = mode == RN_MODE_RECORD || mode == RN_MODE_PRACTICE;
  uint8_t p1 = 0, p2 = 0;
  if (live) {
    // Practice: rn_frame is frozen, so a separate monotonic clock drives tap latching / turbo.
    uint64_t clock = mode == RN_MODE_PRACTICE ? practiceSeq_++ : rn_frame(s);
    rn_input_sample_game(input_, clock, &p1, &p2);
  }
  uint8_t ev = live ? pendingEvents_ : 0;
  pendingEvents_ = 0;
  uint64_t before = rn_frame(s);
  rn_step_info info{};
  if (rn_step(s, p1, p2, ev, &info) != RN_OK) {
    setPaused(true);
    advanceRemaining_ = 0;
    reportError(TR("Couldn’t advance the frame"));
    return false;
  }
  if (mode != RN_MODE_PRACTICE && info.end_of_take && info.frame == before) {
    if (!endOfTake_) notice(TR("End of the take (press “Record” to continue recording from here)"));
    endOfTake_ = true;
    setPaused(true);
    advanceRemaining_ = 0;
    return false;
  }
  endOfTake_ = false;
  if (info.branched) {
    markStructureDirty();
    notice(TR("Started a new take. Use “Back to Previous Take” to return to the old continuation"));
  }
  if (const uint32_t* v = rn_video(s)) {
    if (mode == RN_MODE_PRACTICE) rnf_frame_history_append(history_, v);
    show(v);
  }
  if (audible && audio_) {
    size_t n = 0;
    const int16_t* pcm = rn_audio(s, &n);
    if (n) audio_->push(pcm, n);
  }
  if (mode != RN_MODE_PRACTICE) {
    pendingThumb_ = true;
    pendingThumbFrame_ = info.frame;
  }
  emulatedFrames_ += 1;
  return true;
}

void EmulationController::handleHotkeys(uint32_t e) {
  auto on = [&](uint32_t bit) { return (e & bit) != 0; };
  if (on(RN_HK_PAUSE)) togglePause();
  if (on(RN_HK_FRAME_ADVANCE)) stepFrame(1);
  if (on(RN_HK_STEP_BACK)) stepFrame(-1);
  if (on(RN_HK_SLOW)) toggleSlow();
  if (on(RN_HK_BOOKMARK)) addBookmark();
  if (on(RN_HK_SOFT_RESET)) requestEvent(RN_EV_SOFT_RESET);
  if (on(RN_HK_POWER_CYCLE)) requestEvent(RN_EV_POWER_CYCLE);
  if (on(RN_HK_TOGGLE_MODE)) toggleRecord();
  if (on(RN_HK_SAVE)) save();
  if (on(RN_HK_UNDO_TAKE)) undoTake();
}

// ------------------------------------------------------------------ fast-forward

void EmulationController::beginFastForward() {
  rn_session* s = session_;
  pausedBeforeFF_ = paused_;
  ffTicks_ = 0;
  ffBlocked_ = rn_frame(s) >= rn_take_length(s);
  if (ffBlocked_) {
    notice(rn_take_length(s) == 0 ? TR("Nothing has been recorded yet, so there is nothing to fast-forward")
                                  : TR("End of the recording (fast-forward stops here)"));
    return;
  }
  if (rnf_fast_forward_begin(ff_, s) != RN_OK) {
    reportError(TR("Couldn’t fast-forward"));
    ffBlocked_ = true;
  }
  endOfTake_ = false;
}

void EmulationController::endFastForward() {
  rn_session* s = session_;
  bool wasActive = rnf_fast_forward_active(ff_);
  if (rnf_fast_forward_end(ff_, s) != RN_OK) reportError(TR("Couldn’t return to record mode"));
  fastForward_ = false;
  if (wasActive) {
    // Like rewind: optionally stop where the user let go, so recording never resumes by surprise.
    setPaused(pauseAfterRewind ? true : (pausedBeforeFF_ || rn_frame(s) >= rn_take_length(s)));
  }
  ffBlocked_ = false;
}

void EmulationController::tickFastForward() {
  setMuted(true);
  if (!rnf_fast_forward_active(ff_) || ffBlocked_) return;
  int done = 0, atEnd = 0;
  ffTicks_ += 1;  // the same speed curve as rewind (rnf_hold_speed), forwards
  if (rnf_fast_forward_step(ff_, session_, rnf_hold_speed(ffTicks_), &done, &atEnd) != RN_OK) {
    ffBlocked_ = true;
    setPaused(true);
    reportError(TR("Couldn’t fast-forward"));
    return;
  }
  if (done > 0) publishVideo(true);
  if (atEnd) {
    ffBlocked_ = true;
    setPaused(true);
    notice(TR("Reached the end of the recording (paused here)"));
  }
}

// ------------------------------------------------------------------ practice

void EmulationController::tickPractice() {
  rn_session* s = session_;
  bool hasLen = practiceSlot_ >= 0 && practiceLength_ > 0;
  rnf_practice_action a = rnf_practice_loop_tick(practiceLoop_, nowSeconds(), rn_practice_frame(s), hasLen ? 1 : 0,
                                                 hasLen ? practiceLength_ : 0);
  switch (a.kind) {
    case RNF_PRACTICE_STEP: {
      practiceRewindStarted_ = false;
      bool audible = slow_ == 1;
      setMuted(!audible);
      if (tickCount_ % uint64_t(slow_) == 0) stepOnce(audible);
      else if (flashAltered_) publishVideo(true);
      break;
    }
    case RNF_PRACTICE_BEGIN_HOLD: {
      // Picture stays; a short decaying tail lets the sound end naturally (no click, no black).
      size_t n = 0;
      const int16_t* pcm = rn_audio(s, &n);
      if (n && audio_) {
        std::vector<int16_t> tail(rnf_audio_fade_tail(pcm, n, 4, nullptr, 0));
        rnf_audio_fade_tail(pcm, n, 4, tail.data(), tail.size());
        audio_->push(tail.data(), tail.size());
      }
      break;
    }
    case RNF_PRACTICE_HOLD: break;
    case RNF_PRACTICE_REWIND_FRAME: {
      if (!practiceRewindStarted_) {
        practiceRewindStarted_ = true;
        setMuted(true);
        resetFlashFilter();
      }
      int idx = rnf_practice_history_index(a.back, rnf_frame_history_count(history_));
      if (const uint32_t* f = rnf_frame_history_frame(history_, idx)) show(f);
      break;
    }
    case RNF_PRACTICE_RESTART: {
      practiceRewindStarted_ = false;
      if (practiceSlot_ < 0) return;
      if (rn_practice_goto_a(s, uint32_t(practiceSlot_)) == RN_OK) {
        rnf_frame_history_clear(history_);
        // No publish here: rn_video still holds the last practice frame; the next step shows the
        // frame after A, continuing the rewind motion.
        resetFlashFilter();
      } else {
        int slot = practiceSlot_;
        practiceSlot_ = -1;
        practiceLength_ = 0;
        markStructureDirty();
        notice(TRF("Point A of section %lld can’t be found, so repeating stopped", {slot + 1}));
      }
      break;
    }
  }
}

void EmulationController::startPractice(int slot) {
  rn_session* s = session_;
  if (!s) return;
  if (fastForward_) {
    endFastForward();
    uiFastForwardHeld_ = false;
  }
  rn_mode wasMode = rn_get_mode(s);
  if (rn_practice_goto_a(s, uint32_t(slot)) != RN_OK) {
    rn_practice_slot_info si{};
    if (rn_practice_slot_get(s, uint32_t(slot), &si) == RN_OK && !si.has_a)
      notice(TRF("Point A of section %lld hasn’t been set yet", {slot + 1}));
    else
      reportError(TR("Couldn’t start practicing"));
    return;
  }
  if (wasMode != RN_MODE_PRACTICE) modeBeforePractice_ = wasMode;
  rn_practice_slot_info si{};
  rn_practice_slot_get(s, uint32_t(slot), &si);
  SlotInfo info;
  info.index = slot;
  info.name = si.name ? si.name : "";
  practiceSlot_ = slot;
  practiceLength_ = si.has_b ? si.length_frames : 0;
  rnf_practice_loop_reset(practiceLoop_);
  practiceRewindStarted_ = false;
  rnf_frame_history_clear(history_);
  rnf_step_repeater_reset(stepRepeater_);
  advanceRemaining_ = 0;
  endOfTake_ = false;
  setPaused(false);
  resetFlashFilter();
  publishVideo();
  markStructureDirty();
  notice(si.has_b ? TRF("Practice: %@ (returns to A at B and repeats; nothing is recorded)", {info.displayName()})
                  : TRF("Practice: %@ (B isn’t set, so it doesn’t repeat; nothing is recorded)", {info.displayName()}));
}

void EmulationController::stopPractice() {
  rn_session* s = session_;
  if (!s || rn_get_mode(s) != RN_MODE_PRACTICE) return;
  if (rn_set_mode(s, modeBeforePractice_) != RN_OK) {
    reportError(TR("Couldn’t stop practicing"));
    return;
  }
  practiceSlot_ = -1;
  practiceLength_ = 0;
  rnf_practice_loop_reset(practiceLoop_);
  practiceRewindStarted_ = false;
  rnf_frame_history_release(history_);
  setPaused(true);
  advanceRemaining_ = 0;
  setMuted(true);
  publishVideo();
  markStructureDirty();
  notice(TR("Stopped practicing (the take is unchanged)"));
}

void EmulationController::refreshPracticeLength(int slot) {
  if (slot != practiceSlot_) return;
  rn_practice_slot_info si{};
  rn_practice_slot_get(session_, uint32_t(slot), &si);
  practiceLength_ = si.has_a && si.has_b ? si.length_frames : 0;
  if (!si.has_a) practiceSlot_ = -1;
}

void EmulationController::practiceSetA(int slot) {
  rn_session* s = session_;
  if (!s) return;
  if (rn_practice_set_a(s, uint32_t(slot)) != RN_OK) {
    reportError(TR("Couldn’t set A"));
    return;
  }
  if (rn_get_mode(s) == RN_MODE_PRACTICE) {
    practiceSlot_ = slot;
    rnf_practice_loop_interrupt(practiceLoop_);
  }
  refreshPracticeLength(slot);
  markStructureDirty();
  notice(TRF("Set A of section %lld. Keep playing and set B where it should end", {slot + 1}));
}

void EmulationController::practiceSetAAt(int slot, uint64_t frame) {
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) {
    notice(practiceBlockedText());
    return;
  }
  // A is the machine state at the cursor: go there, take it, come back (paused, silent).
  uint64_t back = rn_frame(s);
  frame = std::min(frame, rn_take_length(s));
  if (frame != back && rn_seek(s, frame) != RN_OK) {
    reportError(TR("Couldn’t move"));
    return;
  }
  if (rn_practice_set_a(s, uint32_t(slot)) == RN_OK) {
    refreshPracticeLength(slot);
    markStructureDirty();
    notice(TRF("A/B %lld: A %@", {slot + 1, timecode(frame)}));
  } else {
    reportError(TR("Couldn’t set A"));
  }
  if (frame != back) rn_seek(s, back);
  setMuted(true);
  publishVideo();
}

void EmulationController::practiceSetB(int slot) {
  rn_session* s = session_;
  if (!s) return;
  rn_status st = rn_practice_set_b(s, uint32_t(slot));
  if (st == RN_OK) {
    refreshPracticeLength(slot);
    markStructureDirty();
    rn_practice_slot_info si{};
    rn_practice_slot_get(s, uint32_t(slot), &si);
    notice(TRF("Set B of section %lld (length %@). Use “Practice” to repeat it", {slot + 1, timecode(si.length_frames)}));
    return;
  }
  switch (st) {
    case RN_ERR_DISCONTINUITY:
      if (practiceSetBFromTake(slot)) return;  // A is on this take: B from take frames
      notice(TR("Set B at a point reached by playing on from A (if you rewound, jumped or switched takes after A, start "
                "again from A or set A again)"));
      break;
    case RN_ERR_NOT_FOUND: notice(TRF("Set A of section %lld first", {slot + 1})); break;
    case RN_ERR_INVALID_ARG: notice(TR("A and B are at the same position. Play a little further before setting B")); break;
    default: reportError(TR("Couldn’t set B")); break;
  }
}

void EmulationController::practiceRename(int slot, const std::string& name) {
  if (!session_) return;
  if (rn_practice_slot_rename(session_, uint32_t(slot), name.c_str()) == RN_OK) markStructureDirty();
  else reportError(TR("Couldn’t rename"));
}

void EmulationController::practiceClear(int slot) {
  if (!session_) return;
  if (rn_practice_slot_clear(session_, uint32_t(slot)) == RN_OK) {
    refreshPracticeLength(slot);
    markStructureDirty();
  } else {
    reportError(TR("Couldn’t clear the section"));
  }
}

bool EmulationController::timelineRange(int slot, rnf_timeline_range* out) {
  for (const rnf_timeline_range& r : visibleRanges())
    if (r.slot == slot) {
      *out = r;
      return true;
    }
  return false;
}

std::vector<rnf_timeline_range> EmulationController::visibleRanges() {
  std::vector<rnf_timeline_range> out;
  if (!session_) return out;
  const SessionStructure& st = structure();
  std::vector<rnf_practice_slot> slots;
  for (const SlotInfo& s : st.slots)
    slots.push_back(rnf_practice_slot{s.index, s.hasA, s.hasB, s.hasTakeFrame, s.takeFrame, s.length, s.takeId});
  std::vector<rn_take_info> takes;
  for (const TakeInfo& t : st.takes)
    takes.push_back(rn_take_info{t.id, t.parentId, t.branchFrame, t.length, t.createdSeq, t.isActive, t.childCount});
  out.resize(slots.size());
  size_t n = rnf_timeline_visible_ranges(slots.data(), slots.size(), takes.data(), takes.size(), rn_active_take(session_),
                                         rn_take_length(session_), out.data(), out.size());
  out.resize(std::min(n, out.size()));
  return out;
}

void EmulationController::practiceSetRange(int slot, uint64_t a, uint64_t b, bool fromMarkers) {
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) {
    notice(practiceBlockedText());
    return;
  }
  if (rn_practice_set_range(s, uint32_t(slot), a, b) != RN_OK) {
    reportError(TR("Couldn’t set the section"));
    return;
  }
  markStructureDirty();
  // The engine restores the picture of the cursor: show it again.
  publishVideo();
  if (fromMarkers) notice(TRF("A/B %lld: A %@ → B %@", {slot + 1, timecode(a), timecode(b)}));
  else
    notice(TRF("Section %lld: A %@ → B %@ (length %@). Click the section to practice it",
               {slot + 1, timecode(a), timecode(b), timecode(b - a)}));
}

void EmulationController::timelineMarkA(int slot) {
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) {
    notice(practiceBlockedText());
    return;
  }
  rnf_timeline_range r{};
  bool has = timelineRange(slot, &r);
  uint64_t a = 0, b = 0;
  char* msg = nullptr;
  switch (rnf_timeline_mark_a(rn_frame(s), has ? &r : nullptr, &a, &b, &msg)) {
    case RNF_MARK_SET_RANGE: practiceSetRange(slot, a, b); break;
    case RNF_MARK_SET_A_ONLY: practiceSetA(slot); break;
    case RNF_MARK_INVALID: notice(msg ? msg : ""); break;
  }
  rnf_string_free(msg);
}

void EmulationController::timelineMarkB(int slot) {
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) {
    notice(practiceBlockedText());
    return;
  }
  rnf_timeline_range r{};
  bool has = timelineRange(slot, &r);
  uint64_t a = 0, b = 0;
  char* msg = nullptr;
  switch (rnf_timeline_mark_b(rn_frame(s), has ? &r : nullptr, &a, &b, &msg)) {
    case RNF_MARK_SET_RANGE: practiceSetRange(slot, a, b); break;
    case RNF_MARK_SET_A_ONLY: practiceSetA(slot); break;
    case RNF_MARK_INVALID: notice(msg ? msg : ""); break;
  }
  rnf_string_free(msg);
}

bool EmulationController::practiceSetBFromTake(int slot) {
  rn_session* s = session_;
  if (!s || rn_get_mode(s) == RN_MODE_PRACTICE) return false;
  rnf_timeline_range r{};
  bool has = timelineRange(slot, &r);
  uint64_t a = 0, b = 0;
  char* msg = nullptr;
  rnf_mark_plan p = rnf_timeline_mark_b(rn_frame(s), has ? &r : nullptr, &a, &b, &msg);
  rnf_string_free(msg);
  if (p != RNF_MARK_SET_RANGE) return false;
  practiceSetRange(slot, a, b);
  return true;
}

// ------------------------------------------------------------------ operations

void EmulationController::requestEvent(uint8_t ev) {
  rn_session* s = session_;
  if (!s) return;
  rn_mode m = rn_get_mode(s);
  if (m == RN_MODE_REPLAY) {
    notice(TR("Reset is only available in record mode (or while practicing)"));
    return;
  }
  pendingEvents_ |= ev;
  if (paused_) advanceRemaining_ += 1;  // apply immediately so the user sees it
  bool power = ev == RN_EV_POWER_CYCLE;
  if (m == RN_MODE_PRACTICE)
    notice(power ? TR("Power cycle (practicing: not recorded)") : TR("Soft reset (practicing: not recorded)"));
  else
    notice(power ? TR("Recording a power cycle") : TR("Recording a soft reset"));
}

void EmulationController::setRecording(bool rec) {
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) stopPractice();
  if (fastForward_) endFastForward();
  if (rn_set_mode(s, rec ? RN_MODE_RECORD : RN_MODE_REPLAY) == RN_OK) endOfTake_ = false;
  else reportError(TR("Couldn’t switch modes"));
}

void EmulationController::toggleRecord() {
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) {
    stopPractice();
    return;
  }
  if (fastForward_) endFastForward();
  rnf_record_toggle_plan plan = rnf_record_toggle(rn_get_mode(s) == RN_MODE_RECORD, rn_frame(s), rn_take_length(s));
  if (!plan.record && rn_take_length(s) == 0) {
    notice(TR("Nothing has been recorded yet"));
    return;
  }
  setRecording(plan.record != 0);
  if (plan.has_seek) seekCommand(plan.seek);
  advanceRemaining_ = 0;
  setPaused(!plan.play);
  if (plan.play) setMuted(true);  // unmuted by the next audible step
  notice(plan.record ? TR("Record mode: recording continues from here with your next input")
                     : TR("Playback mode: plays the recorded take (nothing is recorded)"));
}

void EmulationController::rerecordHere() {
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) stopPractice();
  setRecording(true);
  if (rn_get_mode(s) == RN_MODE_RECORD) {
    setPaused(false);
    slow_ = 1;
    notice(TR("Record mode: recording continues from here with your next input"));
  }
}

void EmulationController::addBookmark(const std::string& name) {
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) {
    notice(practiceBlockedText());
    return;
  }
  std::string n = name.empty() ? TRF("Bookmark %lld (%@)", {(long long)rn_bookmark_count(s) + 1, timecode(rn_frame(s))}) : name;
  uint64_t id = 0;
  if (rn_bookmark_add(s, n.c_str(), &id) == RN_OK) {
    markStructureDirty();
    notice(n);
  } else {
    reportError(TR("Couldn’t add the bookmark"));
  }
}

void EmulationController::gotoBookmark(uint64_t id) {
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) {
    notice(practiceBlockedText());
    return;
  }
  if (rnf_fast_forward_active(ff_)) endFastForward();
  if (rn_bookmark_goto(s, id) != RN_OK) {
    reportError(TR("Couldn’t jump to the bookmark"));
    return;
  }
  setPaused(true);
  setMuted(true);
  endOfTake_ = false;
  markStructureDirty();
  publishVideo();
  pendingThumb_ = true;
  pendingThumbFrame_ = rn_frame(s);
}

void EmulationController::removeBookmark(uint64_t id) {
  if (!session_) return;
  if (rn_bookmark_remove(session_, id) == RN_OK) markStructureDirty();
  else reportError(TR("Couldn’t delete the bookmark"));
}

void EmulationController::renameBookmark(uint64_t id, const std::string& name) {
  if (!session_) return;
  if (rn_bookmark_rename(session_, id, name.c_str()) == RN_OK) markStructureDirty();
  else reportError(TR("Couldn’t rename"));
}

void EmulationController::activateTake(uint64_t id) {
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) {
    notice(practiceBlockedText());
    return;
  }
  if (rnf_fast_forward_active(ff_)) endFastForward();
  if (rn_take_activate(s, id) != RN_OK) {
    reportError(TR("Couldn’t switch takes"));
    return;
  }
  setPaused(true);
  setMuted(true);
  endOfTake_ = false;
  markStructureDirty();
  publishVideo();
  notice(TRF("Switched to take #%llu", {(unsigned long long)id}));
}

void EmulationController::undoTake() {
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) {
    notice(practiceBlockedText());
    return;
  }
  if (rn_undo_depth(s) == 0) {
    notice(TR("There is no previous take to go back to"));
    return;
  }
  if (rnf_fast_forward_active(ff_)) endFastForward();
  if (rn_undo_take_switch(s) != RN_OK) {
    reportError(TR("Couldn’t go back to the previous take"));
    return;
  }
  setPaused(true);
  endOfTake_ = false;
  setMuted(true);
  publishVideo();
  markStructureDirty();
  notice(TR("Went back to the previous take"));
}

bool EmulationController::save() {
  rn_session* s = session_;
  if (!s) return false;
  if (!*rn_session_project_dir(s)) {
    notice(TR("This session has no save location yet. Use “Save As…”"));
    return false;
  }
  if (rn_session_save(s) != RN_OK) {
    reportError(TR("Couldn’t save (your work is still kept in memory)"));
    return false;
  }
  autosaveFailed_ = false;
  if (tempSession) lastFullSave_ = nowSeconds();
  return true;
}

std::string EmulationController::flushForResume(bool fullSave) {
  rn_session* s = session_;
  if (!s || !*rn_session_project_dir(s) || !rn_session_has_unsaved_changes(s)) return "";
  rn_status st = fullSave ? rn_session_save(s) : rn_session_autosave(s);
  if (st != RN_OK) return lastError();
  if (fullSave) lastFullSave_ = nowSeconds();
  lastAutosave_ = nowSeconds();
  autosaveFailed_ = false;
  return "";
}

void EmulationController::applyResume(const rnf_resume_record& r) {
  rn_session* s = session_;
  if (!s) return;
  if (rnf_resume_apply(&r, s) != RN_OK) reportError(TR("Couldn’t move to the previous position"));
  setPaused(true);
  setMuted(true);
  endOfTake_ = false;
  markStructureDirty();
  publishVideo();
  pendingThumb_ = true;
  pendingThumbFrame_ = rn_frame(s);
  updateStatus();
}

std::string EmulationController::resetProject(bool keepPracticeSlots,
                                              const std::function<std::string(const std::string&)>& backup) {
  rn_session* s = session_;
  if (!s) return "";
  if (rn_get_mode(s) == RN_MODE_PRACTICE && rn_set_mode(s, RN_MODE_RECORD) != RN_OK) return lastError();
  if (rnf_fast_forward_active(ff_)) endFastForward();
  std::string dir = rn_session_project_dir(s);
  if (backup && !dir.empty()) {
    // The journal holds everything up to now, so the copy reopens with all of it.
    if (rn_session_has_unsaved_changes(s) && rn_session_autosave(s) != RN_OK)
      return std::string(TR("Couldn’t make a backup copy, so the project was not reset.")) + "\n\n" + lastError();
    std::string err = backup(dir);
    if (!err.empty()) return std::string(TR("Couldn’t make a backup copy, so the project was not reset.")) + "\n\n" + err;
  }
  if (rn_session_reset(s, keepPracticeSlots ? 1 : 0) != RN_OK) {
    // Not committed: the engine restored the previous content.
    std::string err = lastError();
    setPaused(true);
    markStructureDirty();
    publishVideo();
    return err;
  }
  install(s);  // like a new project: empty take at frame 0, record mode, running
  return "";
}

// ------------------------------------------------------------------ housekeeping / status

void EmulationController::afterFrame(double slack) {
  rn_session* s = session_;
  if (!s) return;
  if (pendingThumb_) {
    pendingThumb_ = false;
    if (observer) observer->frameShown(s, pendingThumbFrame_);
  }
  if (!rewinding_ && !fastForward_) maybeAutosave(slack);
}

void EmulationController::maybeAutosave(double slack) {
  rn_session* s = session_;
  double now = nowSeconds();
  if (autosaveInterval <= 0 || !*rn_session_project_dir(s)) return;
  if (!autosaveSoon_ && now - lastAutosave_ < autosaveInterval) return;
  if (slack < 0.008 && !paused_) return;  // try again next frame
  lastAutosave_ = now;
  autosaveSoon_ = false;
  if (!rn_session_has_unsaved_changes(s)) return;
  rn_status st;
  if (tempSession && paused_ && (lastFullSave_ == 0 || now - lastFullSave_ >= 30)) {
    st = rn_session_save(s);
    if (st == RN_OK) lastFullSave_ = now;
  } else {
    st = rn_session_autosave(s);
  }
  if (st == RN_OK) {
    autosaveFailed_ = false;
  } else {
    if (!autosaveFailed_) reportError(TR("Autosave failed (your work is still kept in memory)"));
    autosaveFailed_ = true;
  }
}

void EmulationController::updateStatus() {
  EmuStatus& st = status_;
  rn_session* s = session_;
  if (!s) {
    st = EmuStatus();
    st.paused = paused_;
    return;
  }
  st.hasSession = true;
  st.frame = rn_frame(s);
  st.takeLength = rn_take_length(s);
  st.recording = rnf_fast_forward_shows_recording(ff_, s) != 0;
  st.paused = paused_;
  st.slow = slow_;
  st.rewinding = rewinding_;
  st.fastForward = fastForward_ && rnf_fast_forward_active(ff_) && !ffBlocked_;
  st.endOfTake = endOfTake_;
  st.activeTake = rn_active_take(s);
  st.takeCount = rn_take_count(s);
  st.undoDepth = rn_undo_depth(s);
  st.unsaved = rn_session_has_unsaved_changes(s) != 0;
  if (st.projectPath != rn_session_project_dir(s)) st.projectPath = rn_session_project_dir(s);
  if (st.romPath != rn_session_rom_path(s)) st.romPath = rn_session_rom_path(s);
  st.advancePending = advanceRemaining_;
  st.flashActive = flashActiveShown_;
  st.practicing = rn_get_mode(s) == RN_MODE_PRACTICE;
  if (st.practicing) {
    st.practiceSlot = practiceSlot_;
    st.practiceFrame = rn_practice_frame(s);
    st.practiceLength = practiceSlot_ >= 0 ? practiceLength_ : 0;
    double since = 0;
    st.practiceLooping = rnf_practice_loop_phase(practiceLoop_, &since) != RNF_PHASE_PLAYING;
    st.practiceLoops = rnf_practice_loop_loops(practiceLoop_);
  } else {
    st.practiceSlot = -1;
    st.practiceFrame = st.practiceLength = 0;
    st.practiceLooping = false;
    st.practiceLoops = 0;
  }
}

const SessionStructure& EmulationController::structure() {
  rn_session* s = session_;
  if (!s) {
    if (!structure_.takes.empty() || !structure_.bookmarks.empty() || !structure_.slots.empty()) {
      structure_ = SessionStructure();
      structureVersion_ += 1;
    }
    return structure_;
  }
  uint64_t take = rn_active_take(s);
  // Take lengths / new takes change while recording: refresh ~1/s.
  if (!structureDirty_ && take == lastStructureTake_ && tickCount_ - structureTick_ < 60) return structure_;
  structureDirty_ = false;
  lastStructureTake_ = take;
  structureTick_ = tickCount_;
  SessionStructure st;
  for (size_t i = 0, n = rn_bookmark_count(s); i < n; ++i) {
    rn_bookmark_info b{};
    if (rn_bookmark_get(s, i, &b) != RN_OK) continue;
    st.bookmarks.push_back(BookmarkInfo{b.id, b.frame, b.take_id, b.name ? b.name : "", b.on_active_take != 0});
  }
  for (size_t i = 0, n = rn_take_count(s); i < n; ++i) {
    rn_take_info t{};
    if (rn_take_get(s, i, &t) != RN_OK) continue;
    st.takes.push_back(TakeInfo{t.id, t.parent_id, t.branch_frame, t.length, t.created_seq, t.is_active != 0, t.child_count});
  }
  for (uint32_t i = 0; i < RN_PRACTICE_SLOTS; ++i) {
    rn_practice_slot_info p{};
    rn_practice_slot_get(s, i, &p);
    SlotInfo si;
    si.index = int(i);
    si.hasA = p.has_a != 0;
    si.hasB = p.has_b != 0;
    si.length = p.length_frames;
    si.name = p.name ? p.name : "";
    si.hasTakeFrame = p.has_take_frame != 0;
    si.takeFrame = p.take_frame;
    si.takeId = p.take_id;
    st.slots.push_back(si);
  }
  bool same = st.bookmarks.size() == structure_.bookmarks.size() && st.takes.size() == structure_.takes.size();
  if (same) {
    for (size_t i = 0; i < st.takes.size() && same; ++i)
      same = st.takes[i].length == structure_.takes[i].length && st.takes[i].isActive == structure_.takes[i].isActive;
    for (size_t i = 0; i < st.bookmarks.size() && same; ++i)
      same = st.bookmarks[i].name == structure_.bookmarks[i].name && st.bookmarks[i].onActiveTake == structure_.bookmarks[i].onActiveTake;
    for (size_t i = 0; i < st.slots.size() && i < structure_.slots.size() && same; ++i)
      same = st.slots[i].hasA == structure_.slots[i].hasA && st.slots[i].hasB == structure_.slots[i].hasB &&
             st.slots[i].length == structure_.slots[i].length && st.slots[i].name == structure_.slots[i].name &&
             st.slots[i].takeFrame == structure_.slots[i].takeFrame;
    same = same && st.slots.size() == structure_.slots.size();
  }
  structure_ = std::move(st);
  if (!same) structureVersion_ += 1;
  return structure_;
}

}  // namespace rnl
