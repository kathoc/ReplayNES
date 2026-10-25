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
#if RN_PRODUCT_FLASH_FILTER
  flash_ = rn_flash_filter_new(flashLevel_);
#endif
  ff_ = rnf_fast_forward_new();
  stepRepeater_ = rnf_step_repeater_new();
  practiceLoop_ = rnf_practice_loop_new();
  resumeCountdown_ = rnf_resume_countdown_new();
  reel_ = rnf_reel_new(RNF_REEL_CAPACITY);
  vtr_ = rnf_vtr_new();
  retry_ = rnf_retry_new();
  rnf_vtr_configure(vtr_, vtrEnabled_ ? 1 : 0, flashLevel_);
  power_ = rnf_crt_power_new();
  hiss_ = rnf_crt_hiss_new();
  display_.assign(size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT, 0xFF000000u);
}

EmulationController::~EmulationController() {
  closeSession();
#if RN_PRODUCT_FLASH_FILTER
  rn_flash_filter_free(flash_);
#endif
  rnf_fast_forward_free(ff_);
  rnf_step_repeater_free(stepRepeater_);
  rnf_practice_loop_free(practiceLoop_);
  rnf_resume_countdown_free(resumeCountdown_);
  rnf_reel_free(reel_);
  rnf_vtr_free(vtr_);
  rnf_retry_free(retry_);
  rnf_crt_power_free(power_);
  rnf_crt_hiss_free(hiss_);
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
  if (p) rnf_resume_countdown_cancel(resumeCountdown_);  // a pause (hotkey, menu, step, cancel) aborts it
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
  char* layout = s ? rn_session_input_layout_json(s) : nullptr;
  rnf_input_set_layout_json(layout);
  const bool wasNes = nesLayout_;
  nesLayout_ = std::strcmp(rnf_input_layout_name(), "nes") == 0;
  if (onInputLayout) onInputLayout(layout);
  else if (!nesLayout_ || !wasNes) rn_input_set_layout_json(input_, layout);
  rn_string_free(layout);
  ++mediaGeneration_;
  media_ = {RN_VIDEO_WIDTH, RN_VIDEO_HEIGHT, RN_FPS_NUM, RN_FPS_DEN, 1, 8, 7, 0};
  if (s) rn_session_media_info(s, &media_);
  pictureWidth_ = media_.max_width;
  pictureHeight_ = media_.max_height;
  display_.assign(size_t(pictureWidth_) * pictureHeight_, 0xFF000000u);
#if RN_PRODUCT_FLASH_FILTER
  rn_flash_filter_set_size(flash_, pictureWidth_, pictureHeight_);
#endif
  rnf_reel_free(reel_);
  reel_ = rnf_reel_new_media(RNF_REEL_CAPACITY, &media_);
  if (audio_) {
    const double sampleRate = s ? double(rn_session_samples_before(s, media_.fps_num)) / double(media_.fps_den)
                                : double(RN_SAMPLE_RATE);
    audio_->configureMedia(media_, sampleRate);
  }
  bool fresh = s && rn_take_length(s) == 0 && rn_frame(s) == 0 && (rn_get_mode(s) == RN_MODE_RECORD || rn_get_mode(s) == RN_MODE_LIVE);
  setPaused(!fresh);
  slow_ = 1;
  advanceRemaining_ = 0;
  pendingEvents_ = 0;
  endOfTake_ = false;
  uiRewindHeld_ = uiFastForwardHeld_ = false;
  stepRepeatHeld_ = false;
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
  rnf_reel_release(reel_);
  rnf_resume_countdown_cancel(resumeCountdown_);  // a fresh recording runs at once
  countdown_ = 0;
  rnf_retry_reset(retry_);  // a new / opened / reset session: no retry to undo
  retryNotice_ = false;
  rewindFromPlay_ = false;
  replayButtons_ = 0xFFFF;
  std::memset(replayRecord_.bytes, 0xFF, sizeof replayRecord_.bytes);
  rnf_vtr_reset(vtr_);
  vtrOn_ = false;
  pendingThumb_ = false;
  rnf_crt_power_reset(power_);  // a new session: its pictures from here (frame 0 = no signal)
  rnf_crt_hiss_reset(hiss_);
  stepPending_ = repeatPending_ = false;
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
  signal_ = Signal();
  rnf_retry_reset(retry_);
  rn_input_release_all(input_);
  setMuted(true);
  if (observer) observer->sessionInstalled(nullptr);
  updateStatus();
}

// ------------------------------------------------------------------ picture / flash filter

void EmulationController::setVtrEffect(bool on) {
  vtrEnabled_ = on;
  rnf_vtr_configure(vtr_, on ? 1 : 0, flashLevel_);
}

void EmulationController::setPracticeCountdown(bool on) { rnf_practice_loop_set_countdown(practiceLoop_, on ? 1 : 0); }

void EmulationController::setResumeCountdown(bool on) { rnf_resume_countdown_set_enabled(resumeCountdown_, on ? 1 : 0); }

void EmulationController::setCrtDisplay(bool on) {
  if (on == crtDisplay_) return;
  crtDisplay_ = on;
  if (!on) rnf_crt_hiss_reset(hiss_);  // no CRT, no hiss (it fades in from 0 when it is back)
}

void EmulationController::setFlashLevel(rn_flash_level l) {
#if RN_PRODUCT_FLASH_FILTER
  if (l == flashLevel_) return;
  flashLevel_ = l;
  rnf_vtr_configure(vtr_, vtrEnabled_ ? 1 : 0, l);
  rn_flash_filter_set_level(flash_, l);
  flashAltered_ = false;
  if (l == RN_FLASH_OFF) publishVideo();  // the unfiltered picture right away
#else
  (void)l;
#endif
}

void EmulationController::resetFlashFilter() {
#if RN_PRODUCT_FLASH_FILTER
  rn_flash_filter_reset(flash_);
#endif
  flashAltered_ = false;
}

bool EmulationController::crtAvailable() const {
  rn_video_indices_info indices{};
  return session_ && pictureWidth_ == RN_VIDEO_WIDTH && pictureHeight_ == RN_VIDEO_HEIGHT &&
         media_.rotation == 0 && rn_video_indices(session_, &indices) == RN_OK && indices.codes;
}

void EmulationController::show(const rn_video_frame_info& frame) {
  const uint32_t* v = frame.pixels;
  if (!v || !frame.width || !frame.height || frame.stride < frame.width) return;
  if (pictureWidth_ != frame.width || pictureHeight_ != frame.height) {
    pictureWidth_ = frame.width;
    pictureHeight_ = frame.height;
    display_.resize(size_t(frame.width) * frame.height);
#if RN_PRODUCT_FLASH_FILTER
    rn_flash_filter_set_size(flash_, frame.width, frame.height);
#endif
  }
  signal_ = Signal();
  rn_video_indices_info vi{};
  if (crtAvailable() && frame.stride == RN_VIDEO_WIDTH && v == rn_video(session_) && rn_video_indices(session_, &vi) == RN_OK && vi.codes) {
    signal_.codes = vi.codes;
    signal_.burstPhase = vi.burst_phase;
    signal_.ordinal = vi.frame;
  } else {
    signal_.ordinal = ++pictureOrdinal_;
  }
  int codesOk = 0;
  if (flashLevel_ == RN_FLASH_OFF || frame.width % 16 || frame.height % 16) {
    for (uint32_t y = 0; y < frame.height; ++y)
      std::memcpy(display_.data() + size_t(y) * frame.width, v + size_t(y) * frame.stride, size_t(frame.width) * 4);
    flashAltered_ = false;
#if RN_PRODUCT_FLASH_FILTER
  } else {
    rn_flash_info info{};
    if (signal_.codes) {
      // The PPU codes go through the filter too, so a reduced picture keeps the CRT's RF path (no
      // switch to the RGB input and back while flashes are reduced).
      rn_flash_filter_process_codes(flash_, v, display_.data(), signal_.codes, displayCodes_.data(), &info, &codesOk);
      if (codesOk) signal_.codes = displayCodes_.data();
    } else {
      rn_flash_filter_process_frame(flash_, &frame, display_.data(), &info);
    }
    flashAltered_ = info.altered != 0;
    if (flashAltered_) lastFlashTick_ = tickCount_;
#endif
  }
  // The CRT takes the RGB picture only when the codes do not describe it.
  signal_.flashAltered = flashAltered_ && !codesOk;
  trackReception();
  pictureDirty_ = true;
}

void EmulationController::trackReception() {
  int step;
  if (stepPending_) {
    step = rnf_crt_power_step(power_, stepBefore_, stepFrame_, stepEvents_);
  } else if (repeatPending_) {
    step = rnf_crt_power_tick(power_);
  } else {
    // A jump (seek, rewind, load, the practice sweep). Practice frames are never the machine's frame
    // 0 (no signal is the take's frame 0 only).
    const bool practicing = session_ && rn_get_mode(session_) == RN_MODE_PRACTICE;
    step = rnf_crt_power_show(power_, practicing || !session_ ? UINT64_MAX : rn_frame(session_));
  }
  if (!repeatPending_) snowRepeats_ = 0;  // a new picture: the static snow is encoded again
  stepPending_ = repeatPending_ = false;
  signal_.receptionStep = step;
  signal_.receptionCounter = rnf_crt_power_counter(power_);
  signal_.flashReduced = flashLevel_ != RN_FLASH_OFF;
}

void EmulationController::republishVideo() {
  repeatPending_ = true;
  publishVideo(true);
  repeatPending_ = false;
}

const int16_t* EmulationController::withHiss(const int16_t* pcm, size_t n) {
  if (!crtDisplay_ || !crtAvailable() || media_.audio_channels != 1 || !pcm || n == 0) return nullptr;
  rnf_crt_reception r{};
  rnf_crt_reception_at(rnf_crt_power_current(power_), rnf_crt_power_counter(power_), flashLevel_ != RN_FLASH_OFF ? 1 : 0, &r);
  if (r.hiss == 0 && rnf_crt_hiss_level(hiss_) == 0) return nullptr;
  hissPcm_.assign(pcm, pcm + n);
  rnf_crt_hiss_mix(hiss_, hissPcm_.data(), n, r.hiss);
  return hissPcm_.data();
}

void EmulationController::publishVideo(bool continuous) {
  if (!session_) return;
  rn_video_frame_info frame{};
  if (rn_session_video_frame(session_, &frame) != RN_OK || !frame.pixels) return;
  if (!continuous) resetFlashFilter();
  show(frame);
}

// ------------------------------------------------------------------ seek / step

void EmulationController::seekCommand(uint64_t f) {
  if (live()) { notice(TR("Development core: recording and rewind are unavailable")); return; }
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
  const bool nesTiming = media_.fps_num == RN_FPS_NUM && media_.fps_den == RN_FPS_DEN;
  uint64_t n = uint64_t(std::abs(seconds) * (nesTiming ? 60.0 : 1.0 / framePeriod()));
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
  if (live()) { notice(TR("Development core: recording and rewind are unavailable")); return; }
  if (!session_) return;
  setPaused(true);
  advanceRemaining_ = 0;
  if (rn_get_mode(session_) == RN_MODE_PRACTICE) {
    rnf_practice_loop_interrupt(practiceLoop_);
    if (rn_rewind(session_, n) == RN_OK) {
      rnf_reel_truncate(reel_, rn_practice_frame(session_));
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

bool EmulationController::coreBusy() const {
  return session_ && (!paused_ || rewinding_ || fastForward_ || uiRewindHeld_ || uiFastForwardHeld_ ||
                      advanceRemaining_ > 0 || stepRepeatHeld_ ||
                      rnf_step_repeater_direction(stepRepeater_) != 0);
}

void EmulationController::idleWork(uint32_t budgetFrames) {
  if (!session_ || coreBusy() || budgetFrames == 0) return;
  const rn_mode mode = rn_get_mode(session_);
  if (mode != RN_MODE_RECORD && mode != RN_MODE_REPLAY) return;
  // Cache preparation is optional. An unsuccessful attempt must not interrupt
  // paused navigation with a dialog; normal seek/rewind remains authoritative.
  (void)rn_session_idle_work(session_, budgetFrames);
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
  rn_session* s = session_;
  // Paused at the end of the tape in playback: play continues from there (docs/design/VTR_UX.md:
  // one tape; no restart from the beginning).
  if (paused_ && rn_get_mode(s) == RN_MODE_REPLAY && rn_take_length(s) > 0 && rn_frame(s) >= rn_take_length(s)) {
    rerecordHere();
    return;
  }
  if (paused_) resume();
  else setPaused(true);
}

void EmulationController::resume() {
  if (!paused_) return;
  setPaused(false);
  startResume();
}

void EmulationController::startResume() {
  rn_session* s = session_;
  retryNotice_ = false;
  if (!s || paused_) return;
  rn_mode m = rn_get_mode(s);
  const bool overwrite = rnf_tape_overwrites(m, rn_frame(s), rn_take_length(s)) != 0;
  const bool counting =
      rnf_resume_countdown_start(resumeCountdown_, nowSeconds(), m, rnf_practice_loop_phase(practiceLoop_, nullptr)) != 0;
  // "Retrying from here": under the numeral (the countdown's own pass), else a short notice.
  retryNotice_ = overwrite && counting;
  if (overwrite && !counting) notice(TR("Retrying from here"));
}

bool EmulationController::replayButtonPressed() {
  if (!nesLayout_) {
    rn_input_record now{};
    rn_input_sample_record(input_, rn_frame(session_), &now);
    const bool pressed = rnf_input_record_buttons_pressed(&now, &replayRecord_) != 0;
    replayRecord_ = now;
    return pressed;
  }
  uint8_t p1 = 0, p2 = 0;
  rn_input_sample_game(input_, rn_frame(session_), &p1, &p2);
  const uint16_t now = uint16_t(p1 | (p2 << 8));
  const uint16_t pressed = uint16_t(now & ~replayButtons_);  // held since before watching: no press
  replayButtons_ = now;
  return pressed != 0;
}

void EmulationController::followGesture(int dir, bool repeat) {
  if (!session_ || dir == 0) return;
  if (!repeat) {
    followFromPlay_ = !paused_;
    if (followFromPlay_) {
      if (dir > 0) setPaused(true);
      else toggleSlow();
      return;
    }
  } else if (followFromPlay_) {
    return;
  }
  step(dir);  // paused: frame step (back / forward)
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
  bool rewindHeld = !live() && (uiRewindHeld_ || (held & RN_HK_REWIND) != 0);
  bool ffHeld = !live() && (uiFastForwardHeld_ || (held & RN_HK_FAST_FORWARD) != 0);
  bool wantRewind = rewindHeld;
  // Rewind / fast-forward held during the resume countdown: back to paused (they then act as when
  // paused; releasing them never starts a countdown).
  if ((rewindHeld || ffHeld) && rnf_resume_countdown_active(resumeCountdown_)) setPaused(true);
  if (practicing) {
    // L2 + R2 together: back to A at once (the same return as at B). While that chord is held and
    // during the return, L2 does not rewind the run.
    // Only while playing: paused (the seek bar, the menu, a dialog) the chord latches but does nothing.
    int rw = 0;
    if (rnf_practice_loop_shoulders(practiceLoop_, nowSeconds(), practiceSlot_ >= 0 && !paused_ ? 1 : 0,
                                    rewindHeld ? 1 : 0, ffHeld ? 1 : 0, &rw)) {
      if (rewinding_) {
        rewinding_ = false;
        rewindTicks_ = 0;
      }
      practiceRewindStarted_ = false;
      // The sound stops with a short fade, as at B.
      size_t n = 0;
      const int16_t* pcm = rn_audio(s, &n);
      if (n && audio_ && slow_ == 1) {
        const size_t count = rnf_audio_fade_tail_with_channels(pcm, n, 4, media_.audio_channels, nullptr, 0);
        std::vector<int16_t> tail(count * media_.audio_channels);
        rnf_audio_fade_tail_with_channels(pcm, n, 4, media_.audio_channels, tail.data(), count);
        audio_->push(tail.data(), count, media_.audio_channels);
      }
    }
    wantRewind = rw != 0;
  }
  bool wantFF = !wantRewind && !practicing && ffHeld;
  if (wantFF && !fastForward_) beginFastForward();
  if (!wantFF && fastForward_) endFastForward(!wantRewind);
  fastForward_ = wantFF;
  // Watching the tape (playback, playing): a game button plays from here (RECORD, the countdown).
  if (rn_get_mode(s) == RN_MODE_REPLAY && !paused_ && !modalHold_ && !wantRewind && !fastForward_ &&
      !rnf_resume_countdown_active(resumeCountdown_)) {
    if (replayButtonPressed()) rerecordHere();
  } else {
    replayButtons_ = 0xFFFF;
    std::memset(replayRecord_.bytes, 0xFF, sizeof replayRecord_.bytes);
  }

  bool emulatedLive = false;
  if (wantRewind) {
    if (!rewinding_) {
      resetFlashFilter();  // rewind start: a new continuous (backwards) sequence
      rewindFromPlay_ = !paused_ && !practicing;
    }
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
        if (practicing) rnf_reel_truncate(reel_, rn_practice_frame(s));
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
      if (!practicing) rewindStops_ += 1;
      // Released: paused (Settings), or - a hold that started during play - the resume countdown,
      // then play (from a cursor before the end: a retry, with its notice). A hold started while
      // paused ends paused.
      if (pauseAfterRewind) setPaused(true);
      else if (rewindFromPlay_ && !paused_) startResume();
      rewindFromPlay_ = false;
    }
    bool resumeCounting = !paused_ && tickResumeCountdown();
    if (paused_) {
      int d = rnf_step_repeater_tick(stepRepeater_);
      if (d != 0) stepFrame(d);
    }
    if (resumeCounting) {
      // 3, 2, 1 over the paused picture (tickResumeCountdown): nothing is emulated.
    } else if (fastForward_) {
      tickFastForward();
    } else if (practicing && !paused_ && modalHold_) {
      setMuted(true);  // waits for the dialog (the loop does not count the time)
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
        // Paused steps are navigation: inside the take they replay it (never a new take).
        if (!stepOnce(audible, paused_)) break;
        stepped += 1;
      }
      emulatedLive = stepped > 0 && rn_get_mode(s) != RN_MODE_REPLAY;
      // A frame held back by the flash filter while nothing new is emulated (pause, slow
      // motion) is re-filtered every tick, so the picture settles on the real frame.
      if (stepped == 0 && flashAltered_ && !rewinding_) republishVideo();
    }
  }
  // CRT display at the machine's frame 0 (not powered: no signal): the snow animates while nothing
  // is emulated, a picture per tick (the NES frame rate). Flash reduction: the snow is static, but
  // the tube needs a few pictures of it before the previous picture's phosphor light is gone
  // (kStaticSnowRepeats after the jump, then nothing).
  constexpr int kStaticSnowRepeats = 12;
  if (crtDisplay_ && crtAvailable() && !pictureDirty_ && !vtrOn_ && rnf_crt_power_current(power_) == RNF_CRT_RECEPTION_NO_SIGNAL &&
      (flashLevel_ == RN_FLASH_OFF || snowRepeats_ < kStaticSnowRepeats)) {
    snowRepeats_ += 1;
    rnf_crt_power_tick(power_);
    signal_.receptionCounter = rnf_crt_power_counter(power_);
    pictureDirty_ = true;
  }
  // Paused in the countdown (the seek bar, the menu): the numeral goes; it comes back on resume.
  if (paused_ && countdown_ != 0) countdown_ = 0;
  tickVtr();
  bool active = flashLevel_ != RN_FLASH_OFF && lastFlashTick_ != 0 && tickCount_ - lastFlashTick_ < 45;
  flashActiveShown_ = active;
  updateStatus();
  t.emulated = emulatedLive;
  t.newPicture = pictureDirty_;
  pictureDirty_ = false;
  return t;
}

bool EmulationController::stepOnce(bool audible, bool frameStep) {
  rn_session* s = session_;
  rn_mode mode = rn_get_mode(s);
  bool live = mode == RN_MODE_RECORD || mode == RN_MODE_PRACTICE || mode == RN_MODE_LIVE;
  uint8_t p1 = 0, p2 = 0;
  rn_input_record record{};
  if (live) {
    // Practice: rn_frame is frozen, so a separate monotonic clock drives tap latching / turbo.
    uint64_t clock = mode == RN_MODE_PRACTICE ? practiceSeq_++ : rn_frame(s);
    if (nesLayout_) {
      rn_input_sample_game(input_, clock, &p1, &p2);
      // Apply buttons typed ahead during the countdown only on the first live frame.
      if (!frameStep) rnf_resume_countdown_apply(resumeCountdown_, &p1, &p2);
    } else {
      rn_input_sample_record(input_, clock, &record);
      if (!frameStep) rnf_resume_countdown_apply_record(resumeCountdown_, &record);
    }
  }
  uint8_t ev = live ? pendingEvents_ : 0;
  pendingEvents_ = 0;
  uint64_t before = rn_frame(s);
  const uint64_t takeBefore = rn_active_take(s);
  // Live play goes through the retry bookkeeping (undo window, burst collapse); frame steps replay.
  const bool overwrite = !frameStep && rnf_tape_overwrites(mode, before, rn_take_length(s));
  rn_step_info info{};
  rn_status st;
  if (nesLayout_) {
    st = mode == RN_MODE_LIVE ? rn_step(s, p1, p2, ev, &info) : frameStep ? rnf_transport_step(s, p1, p2, ev, &info)
                             : rnf_retry_step(retry_, s, nowSeconds(), p1, p2, ev, &info);
  } else {
    record.bytes[2] = ev;
    st = mode == RN_MODE_LIVE ? rn_step_record(s, &record, &info)
         : frameStep ? rnf_transport_step_record(s, &record, &info)
                     : rnf_retry_step_record(retry_, s, nowSeconds(), &record, &info);
  }
  if (st != RN_OK) {
    setPaused(true);
    advanceRemaining_ = 0;
    reportError(TR("Couldn’t advance the frame"));
    return false;
  }
  if (mode != RN_MODE_PRACTICE && info.end_of_take && info.frame == before) {
    endOfTake_ = true;  // "End of tape" (the status label); resuming there plays on
    setPaused(true);
    advanceRemaining_ = 0;
    return false;
  }
  endOfTake_ = false;
  // A retry: a new take, or (a rapid retry inside the attempt it abandons) the same take cut at the
  // cursor. Takes are never shown; the structure / thumbnails follow.
  if (info.branched || info.take_id != takeBefore) markStructureDirty();
  else if (overwrite && observer) observer->takeCut(s, before);
  rn_video_frame_info frame{};
  if (rn_session_video_frame(s, &frame) == RN_OK && frame.pixels) {
    if (mode == RN_MODE_PRACTICE) rnf_reel_offer_frame(reel_, rn_practice_frame(s), &frame);
    // The CRT reception: a step from `before` (practice: not a take frame) with the events applied.
    stepPending_ = true;
    stepBefore_ = mode == RN_MODE_PRACTICE ? UINT64_MAX : before;
    stepFrame_ = info.frame;
    stepEvents_ = info.events;
    show(frame);
  }
  if (audible && audio_) {
    size_t n = 0;
    const int16_t* pcm = rn_audio(s, &n);
    if (const int16_t* h = withHiss(pcm, n)) pcm = h;
    if (n) audio_->push(pcm, n, media_.audio_channels);
  }
  if (mode != RN_MODE_PRACTICE && mode != RN_MODE_LIVE) {
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
  if (on(RN_HK_UNDO_TAKE)) undoRetry();
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

void EmulationController::endFastForward(bool released) {
  rn_session* s = session_;
  bool wasActive = rnf_fast_forward_active(ff_);
  if (rnf_fast_forward_end(ff_, s) != RN_OK) reportError(TR("Couldn’t return to record mode"));
  fastForward_ = false;
  if (wasActive) {
    // Like rewind: optionally stop where the user let go. A hold started during play that ends
    // before the end of the tape resumes with the countdown (and the overwrite notice).
    bool atEnd = rn_frame(s) >= rn_take_length(s);
    if (released && !pauseAfterRewind && !pausedBeforeFF_ && !atEnd) {
      setPaused(false);
      startResume();
    } else {
      setPaused(pauseAfterRewind ? true : (pausedBeforeFF_ || atEnd));
    }
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
    endOfTake_ = true;  // "End of tape" (the status label)
  }
}

// ------------------------------------------------------------------ practice

void EmulationController::tickPractice() {
  rn_session* s = session_;
  bool hasLen = practiceSlot_ >= 0 && practiceLength_ > 0;
  rnf_practice_action a = rnf_practice_loop_tick(practiceLoop_, nowSeconds(), rn_practice_frame(s), hasLen ? 1 : 0,
                                                 hasLen ? practiceLength_ : 0);
  if (a.kind != RNF_PRACTICE_COUNTDOWN) countdown_ = 0;
  switch (a.kind) {
    case RNF_PRACTICE_STEP: {
      practiceRewindStarted_ = false;
      bool audible = slow_ == 1;
      setMuted(!audible);
      if (tickCount_ % uint64_t(slow_) == 0) stepOnce(audible);
      else if (flashAltered_) republishVideo();
      break;
    }
    case RNF_PRACTICE_BEGIN_HOLD: {
      // Picture stays; a short decaying tail lets the sound end naturally (no click, no black).
      size_t n = 0;
      const int16_t* pcm = rn_audio(s, &n);
      if (n && audio_) {
        const size_t count = rnf_audio_fade_tail_with_channels(pcm, n, 4, media_.audio_channels, nullptr, 0);
        std::vector<int16_t> tail(count * media_.audio_channels);
        rnf_audio_fade_tail_with_channels(pcm, n, 4, media_.audio_channels, tail.data(), count);
        audio_->push(tail.data(), count, media_.audio_channels);
      }
      dropInput();
      break;
    }
    case RNF_PRACTICE_HOLD: dropInput(); break;
    case RNF_PRACTICE_REWIND_FRAME: {
      // The 1 s sweep back to A over the reel (from B or wherever L2 + R2 was pressed).
      if (!practiceRewindStarted_) {
        practiceRewindStarted_ = true;
        setMuted(true);
        resetFlashFilter();
      }
      int idx = rnf_reel_sweep_index(reel_, a.back);
      rn_video_frame_info frame{};
      if (idx >= 0 && rnf_reel_video_frame(reel_, idx, &frame)) show(frame);
      dropInput();
      break;
    }
    case RNF_PRACTICE_RESTART: {
      practiceRewindStarted_ = false;
      dropInput();
      if (practiceSlot_ < 0) return;
      if (rn_practice_goto_a(s, uint32_t(practiceSlot_)) == RN_OK) {
        arriveAtA();
      } else {
        int slot = practiceSlot_;
        practiceSlot_ = -1;
        practiceLength_ = 0;
        rnf_practice_loop_interrupt(practiceLoop_);
        markStructureDirty();
        notice(TRF("Point A of section %lld can’t be found, so repeating stopped", {slot + 1}));
      }
      break;
    }
    case RNF_PRACTICE_COUNTDOWN:
      // The picture at A stays; nothing reaches the game.
      countdown_ = a.count;
      countdownFraction_ = a.fraction;
      setMuted(true);
      dropInput();
      break;
  }
}

void EmulationController::arriveAtA() {
  rn_session* s = session_;
  rnf_reel_clear(reel_);
  // A's own picture (the frame after A, emulated and taken back: the run does not move), with its
  // PPU codes: the CRT shows it like any frame (the burst phase is part of the machine state, and a
  // still after a discontinuity is drawn as held).
  rnf_practice_preview_a(s);
  resetFlashFilter();
  publishVideo(true);
}

void EmulationController::dropInput() {
  if (!nesLayout_) {
    rn_input_record ignored{};
    rn_input_sample_record(input_, practiceSeq_++, &ignored);
    return;
  }
  uint8_t p1 = 0, p2 = 0;
  rn_input_sample_game(input_, practiceSeq_++, &p1, &p2);
}

bool EmulationController::tickResumeCountdown() {
  rn_session* s = session_;
  if (!s) return false;
  if (!rnf_resume_countdown_active(resumeCountdown_)) {
    // Not counting (ended, cancelled, switched off): outside practice no other countdown is shown.
    if (rn_get_mode(s) != RN_MODE_PRACTICE) countdown_ = 0;
    return false;
  }
  if (modalHold_) {
    rnf_resume_countdown_hold(resumeCountdown_, nowSeconds());  // waits at its start behind a dialog
    countdown_ = 0;
  } else {
    rnf_resume_countdown_state st = rnf_resume_countdown_tick(resumeCountdown_, nowSeconds());
    if (st.done) {
      countdown_ = 0;
      return false;  // play on in this same tick: "1" is followed by the first frame at once
    }
    countdown_ = st.count;
    countdownFraction_ = st.fraction;
  }
  setMuted(true);
  // Type-ahead (docs/design/VTR_UX.md): the game input is sampled and handed to the countdown; a
  // button held through counts from the first frame, one pressed and let go meanwhile is pressed on
  // it (stepOnce: rnf_resume_countdown_apply).
  uint8_t p1 = 0, p2 = 0;
  const uint64_t clock = rn_get_mode(s) == RN_MODE_PRACTICE ? practiceSeq_++ : rn_frame(s);
  if (nesLayout_) {
    rn_input_sample_game(input_, clock, &p1, &p2);
    rnf_resume_countdown_input(resumeCountdown_, p1, p2);
  } else {
    rn_input_record record{};
    rn_input_sample_record(input_, clock, &record);
    rnf_resume_countdown_input_record(resumeCountdown_, &record);
  }
  return true;
}

void EmulationController::abortCountdown() {
  if (session_ && rnf_resume_countdown_active(resumeCountdown_)) {
    setPaused(true);  // the resume countdown: back to paused
    return;
  }
  if (!session_ || rn_get_mode(session_) != RN_MODE_PRACTICE) return;
  if (rnf_practice_loop_phase(practiceLoop_, nullptr) != RNF_PHASE_COUNTDOWN) return;
  stopPractice();
}

// Turns a w x h picture by `turns` quarter turns clockwise into out (w * h pixels, may not alias);
// returns the turned picture's width.
static uint32_t turnQuarter(const uint32_t* in, uint32_t w, uint32_t h, uint32_t turns, uint32_t* out) {
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t v = in[size_t(y) * w + x];
      if (turns == 1) out[size_t(x) * h + (h - 1 - y)] = v;                  // h wide, w high
      else if (turns == 2) out[size_t(h - 1 - y) * w + (w - 1 - x)] = v;
      else if (turns == 3) out[size_t(w - 1 - x) * h + y] = v;              // h wide, w high
      else out[size_t(y) * w + x] = v;
    }
  return turns % 2 ? h : w;
}

// The VTR effect over the picture: the return sweep, L2 rewind, R2 fast-forward (lighter). Runs only
// while it is active (fading in / out); otherwise display_ is untouched (zero cost).
void EmulationController::tickVtr() {
  rnf_vtr_kind want = RNF_VTR_NONE;
  if (session_) {
    double since = 0;
    bool practicing = rn_get_mode(session_) == RN_MODE_PRACTICE;
    if (practicing && rnf_practice_loop_phase(practiceLoop_, &since) == RNF_PHASE_REWINDING) want = RNF_VTR_RETURN;
    else if (rewinding_) want = RNF_VTR_REWIND;
    else if (fastForward_ && rnf_fast_forward_active(ff_) && !ffBlocked_) want = RNF_VTR_FAST_FORWARD;
  }
  if (want == RNF_VTR_NONE && !vtrOn_ && !rnf_vtr_active(vtr_)) return;
  rnf_vtr_params p = rnf_vtr_tick(vtr_, nowSeconds(), want);
  if (p.strength > 0) {
    if (!vtrOn_ || pictureDirty_) {
      // A new clean picture this tick (or the first): it is the effect's source from now on.
      vtrBase_.assign(display_.begin(), display_.end());
      vtrBaseSignal_ = signal_;
      vtrOn_ = true;
    }
    const uint32_t turns = media_.rotation % 4;
    if (turns == 0) {
      rn_video_frame_info frame{vtrBase_.data(), pictureWidth_, pictureHeight_, pictureWidth_};
      rnf_vtr_apply_frame(&frame, display_.data(), pictureWidth_, &p);
    } else {
      // A tape's noise runs along the lines on the screen: a rotated game (a vertical shooter) is
      // turned to the screen's orientation for the effect and back, or its bands would run upright.
      uint32_t w = pictureWidth_, h = pictureHeight_;
      vtrTurned_.resize(size_t(w) * h);
      vtrOut_.resize(size_t(w) * h);
      uint32_t tw = turnQuarter(vtrBase_.data(), w, h, turns, vtrTurned_.data());
      uint32_t th = uint32_t(vtrTurned_.size() / tw);
      rn_video_frame_info frame{vtrTurned_.data(), tw, th, tw};
      rnf_vtr_apply_frame(&frame, vtrOut_.data(), tw, &p);
      turnQuarter(vtrOut_.data(), tw, th, (4 - turns) % 4, display_.data());
    }
    signal_ = vtrBaseSignal_;
    signal_.codes = nullptr;  // the CRT takes the altered picture (RGB), not the PPU codes
    signal_.ordinal = ++pictureOrdinal_;
    pictureDirty_ = true;
  } else if (vtrOn_) {
    vtrOn_ = false;
    if (!pictureDirty_) {
      display_.assign(vtrBase_.begin(), vtrBase_.end());
      signal_ = vtrBaseSignal_;
      pictureDirty_ = true;
    }
  }
}

void EmulationController::startPractice(int slot) {
  if (live()) { notice(TR("Development core: recording and rewind are unavailable")); return; }
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
  rnf_step_repeater_reset(stepRepeater_);
  advanceRemaining_ = 0;
  endOfTake_ = false;
  setPaused(false);
  setMuted(true);
  arriveAtA();
  rnf_practice_loop_arrive(practiceLoop_, nowSeconds());  // the countdown first, when it is on
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
  rnf_reel_release(reel_);
  countdown_ = 0;
  setPaused(true);
  advanceRemaining_ = 0;
  setMuted(true);
  publishVideo();
  markStructureDirty();
  notice(TR("Practice stopped"));  // back where the take was before practice
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
    rnf_reel_clear(reel_);  // the counter starts again at the new A
  }
  refreshPracticeLength(slot);
  markStructureDirty();
  notice(TRF("Set A of section %lld. Keep playing and set B where it should end", {slot + 1}));
}

void EmulationController::practiceSetAAt(int slot, uint64_t frame) {
  if (live()) { notice(TR("Development core: recording and rewind are unavailable")); return; }
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
    notice(TRF("A/B %lld: A %@", {slot + 1, timecode(frame, framePeriod())}));
  } else {
    reportError(TR("Couldn’t set A"));
  }
  if (frame != back) rn_seek(s, back);
  setMuted(true);
  publishVideo();
}

void EmulationController::practiceSetB(int slot) {
  if (live()) { notice(TR("Development core: recording and rewind are unavailable")); return; }
  rn_session* s = session_;
  if (!s) return;
  rn_status st = rn_practice_set_b(s, uint32_t(slot));
  if (st == RN_OK) {
    refreshPracticeLength(slot);
    markStructureDirty();
    rn_practice_slot_info si{};
    rn_practice_slot_get(s, uint32_t(slot), &si);
    notice(TRF("Set B of section %lld (length %@). Use “Practice” to repeat it", {slot + 1, timecode(si.length_frames, framePeriod())}));
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
  if (live()) { notice(TR("Development core: recording and rewind are unavailable")); return; }
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
  if (fromMarkers) notice(TRF("A/B %lld: A %@ → B %@", {slot + 1, timecode(a, framePeriod()), timecode(b, framePeriod())}));
  else
    notice(TRF("Section %lld: A %@ → B %@ (length %@). Click the section to practice it",
               {slot + 1, timecode(a, framePeriod()), timecode(b, framePeriod()), timecode(b - a, framePeriod())}));
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
  if (live() && (ev & RN_EV_SOFT_RESET)) { notice(TR("Development core: soft reset is unavailable")); return; }
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
  if (m == RN_MODE_LIVE)
    notice(TR("Power cycle (development; not recorded)"));
  else if (m == RN_MODE_PRACTICE)
    notice(power ? TR("Power cycle (practicing: not recorded)") : TR("Soft reset (practicing: not recorded)"));
  else
    notice(power ? TR("Recording a power cycle") : TR("Recording a soft reset"));
}

void EmulationController::setRecording(bool rec) {
  if (live()) { notice(TR("Development core: recording and rewind are unavailable")); return; }
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) stopPractice();
  if (fastForward_) endFastForward();
  if (rn_set_mode(s, rec ? RN_MODE_RECORD : RN_MODE_REPLAY) == RN_OK) endOfTake_ = false;
  else reportError(TR("Couldn’t switch modes"));
}

void EmulationController::toggleRecord() {
  if (live()) { notice(TR("Development core: recording and rewind are unavailable")); return; }
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
  // No notice: the status label says "Playback" while watching (docs/design/VTR_UX.md).
}

void EmulationController::rerecordHere() {
  if (live()) { notice(TR("Development core: recording and rewind are unavailable")); return; }
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) stopPractice();
  setRecording(true);
  if (rn_get_mode(s) == RN_MODE_RECORD) {
    slow_ = 1;
    endOfTake_ = false;
    advanceRemaining_ = 0;
    // The resume countdown first (playing from here is live play), with "Retrying from here" before
    // the end of the tape.
    if (paused_) resume();
    else startResume();
  }
}

void EmulationController::addBookmark(const std::string& name) {
  if (live()) { notice(TR("Development core: recording and rewind are unavailable")); return; }
  rn_session* s = session_;
  if (!s) return;
  if (rn_get_mode(s) == RN_MODE_PRACTICE) {
    notice(practiceBlockedText());
    return;
  }
  std::string n = name.empty() ? TRF("Bookmark %lld (%@)", {(long long)rn_bookmark_count(s) + 1, timecode(rn_frame(s), framePeriod())}) : name;
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

bool EmulationController::undoRetry() {
  rn_session* s = session_;
  if (!s) return false;
  double now = nowSeconds();
  if (!rnf_retry_can_undo(retry_, now, rn_active_take(s), rn_get_mode(s))) return false;
  if (rnf_fast_forward_active(ff_)) endFastForward();
  // Back to the continuation from before the retry (the attempt is dropped), paused there.
  rn_status st = rnf_retry_apply_undo(retry_, s, now);
  setPaused(true);
  advanceRemaining_ = 0;
  endOfTake_ = false;
  setMuted(true);
  publishVideo();
  markStructureDirty();
  pendingThumb_ = true;
  pendingThumbFrame_ = rn_frame(s);
  if (st != RN_OK) {
    reportError(TR("Couldn’t go back"));
    return false;
  }
  notice(TR("Back to before the retry"));
  return true;
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
  rnf_retry_reset(retry_);
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
  st.live = live();
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
  // The practice countdown, or the resume countdown (in any mode it applies in).
  st.countdown = countdown_;
  st.countdownFraction = countdownFraction_;
  st.retryNotice = retryNotice_ && countdown_ > 0 && rnf_resume_countdown_active(resumeCountdown_);
  st.resumeCountdown = rnf_resume_countdown_active(resumeCountdown_) != 0;
  st.canUndo = rnf_retry_can_undo(retry_, nowSeconds(), st.activeTake, rn_get_mode(s)) != 0;
  st.rewindStops = rewindStops_;
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
  st.atTapeEnd = !st.practicing && st.takeLength > 0 && st.frame >= st.takeLength;
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
