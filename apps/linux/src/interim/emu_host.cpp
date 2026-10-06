// SPDX-License-Identifier: GPL-2.0-or-later
#include "interim/emu_host.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

#include "host_clock.h"

namespace fs = std::filesystem;

namespace rnl::interim {

namespace {
std::string timestamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H-%M-%S", &tm);
  return buf;
}
/// Keeps a temporary project by moving it into the projects folder (copy across file systems).
void keepProject(const std::string& from, const std::string& projectsDir, const std::string& stem) {
  std::error_code ec;
  fs::create_directories(projectsDir, ec);
  std::string base = projectsDir + "/" + (stem.empty() ? std::string("Session") : stem) + " " + timestamp();
  std::string to = base + ".nesrec";
  for (int i = 2; fs::exists(to, ec); ++i) to = base + " " + std::to_string(i) + ".nesrec";
  fs::rename(from, to, ec);
  if (ec) {
    std::error_code ec2;
    fs::copy(from, to, fs::copy_options::recursive, ec2);
    if (!ec2) fs::remove_all(from, ec2);
  }
}
}  // namespace

EmuHost::EmuHost() {
  input_ = rn_input_new();
  flash_ = rn_flash_filter_new(flashLevel_);
  display_.assign(size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT, 0xFF000000u);
}

EmuHost::~EmuHost() {
  closeSession(true);
  if (flash_) rn_flash_filter_free(flash_);
  if (input_) rn_input_free(input_);
}

void EmuHost::setNotice(const std::string& n) {
  notice_ = n;
  noticeTime_ = nowSeconds();
}

void EmuHost::reportError(const char* what) { setNotice(std::string(what) + ": " + rn_last_error()); }

std::string EmuHost::romName() const {
  if (!session_) return {};
  return fs::path(rn_session_rom_path(session_)).stem().string();
}

bool EmuHost::openRom(const std::string& rom, const std::string& tempProject, const std::string& projectsDir,
                      std::string* error) {
  std::error_code ec;
  if (session_) {
    bool hasContent = rn_take_length(session_) > 0 || rn_bookmark_count(session_) > 0;
    std::string stem = romName();
    closeSession(true);
    if (hasContent) keepProject(tempProject, projectsDir, stem);
    else fs::remove_all(tempProject, ec);
  } else if (fs::exists(tempProject, ec)) {
    keepProject(tempProject, projectsDir, "");  // last run's session, never resumed: keep it
  }
  fs::remove_all(tempProject, ec);
  fs::create_directories(fs::path(tempProject).parent_path(), ec);
  rn_session* s = nullptr;
  if (rn_session_new(rom.c_str(), tempProject.c_str(), nullptr, &s) != RN_OK) {
    *error = rn_last_error();
    return false;
  }
  session_ = s;
  paused_ = false;
  slow_ = SlowRate::normal;
  advanceRemaining_ = 0;
  resetFlash();
  publishPicture(nullptr);
  setNotice("Recording " + romName());
  return true;
}

bool EmuHost::resume(const std::string& tempProject, std::string* error) {
  closeSession(true);
  rn_session* s = nullptr;
  if (rn_session_open(tempProject.c_str(), nullptr, nullptr, &s) != RN_OK) {
    *error = rn_last_error();
    return false;
  }
  session_ = s;
  paused_ = true;
  slow_ = SlowRate::normal;
  resetFlash();
  publishPicture(nullptr);
  setNotice(rn_session_recovered(s) ? "Recovered unsaved work (paused)" : "Resumed (paused)");
  return true;
}

void EmuHost::closeSession(bool save) {
  if (!session_) return;
  if (ff_.active) ff_.end(session_);
  if (save && rn_session_save(session_) != RN_OK) std::fprintf(stderr, "save failed: %s\n", rn_last_error());
  rn_session_close(session_);
  session_ = nullptr;
  rn_input_release_all(input_);
}

void EmuHost::resetFlash() {
  if (flash_) rn_flash_filter_reset(flash_);
  flashAltered_ = false;
}

void EmuHost::setFlashLevel(rn_flash_level level) {
  flashLevel_ = level;
  if (flash_) rn_flash_filter_set_level(flash_, level);
  flashAltered_ = false;
}

void EmuHost::publishPicture(Tick* t) {
  if (!session_) return;
  rn_flash_info info{};
  if (flash_) rn_flash_filter_process(flash_, rn_video(session_), display_.data(), &info);
  flashAltered_ = info.altered != 0;
  pictureDirty_ = true;
  (void)t;
}

void EmuHost::setPaused(bool p) {
  if (p && !paused_) stepRepeater_.reset();
  paused_ = p;
}

void EmuHost::togglePause() {
  if (!session_) return;
  setPaused(!paused_);
  if (!paused_ && rn_get_mode(session_) == RN_MODE_REPLAY && rn_frame(session_) >= rn_take_length(session_) &&
      rn_take_length(session_) > 0) {
    rn_seek(session_, 0);  // play again from the start (RecordToggle.shouldRestartOnPlay)
    resetFlash();
  }
}

void EmuHost::stepFrame(int dir) {
  if (!session_) return;
  setPaused(true);
  if (dir > 0) {
    advanceRemaining_ += 1;
  } else if (rn_frame(session_) > 0) {
    if (rn_rewind(session_, 1) != RN_OK) reportError("Step back failed");
    resetFlash();
    publishPicture(nullptr);
  }
}

void EmuHost::toggleSlow() {
  slow_ = toggled(slow_);
  setNotice(slow_ == SlowRate::normal ? "Normal speed" : "Slow 1/2");
}

void EmuHost::toggleRecord() {
  if (!session_) return;
  RecordTogglePlan p = planRecordToggle(rn_get_mode(session_) == RN_MODE_RECORD, rn_frame(session_), rn_take_length(session_));
  if (p.record) {
    if (rn_set_mode(session_, RN_MODE_RECORD) != RN_OK) return reportError("Couldn't record");
    paused_ = false;
    slow_ = SlowRate::normal;
    setNotice("Recording");
    return;
  }
  if (!p.play) return setNotice("Nothing has been recorded yet");
  if (rn_set_mode(session_, RN_MODE_REPLAY) != RN_OK) return reportError("Couldn't play back");
  if (p.seekToStart) { rn_seek(session_, 0); resetFlash(); }
  paused_ = false;
  setNotice("Playback");
}

void EmuHost::save() {
  if (!session_) return;
  if (rn_session_save(session_) != RN_OK) reportError("Save failed");
  else setNotice("Saved");
}

void EmuHost::handleHotkeys(uint32_t edges) {
  auto on = [&](uint32_t bit) { return (edges & bit) != 0; };
  if (on(RN_HK_PAUSE)) togglePause();
  if (on(RN_HK_FRAME_ADVANCE)) stepFrame(1);
  if (on(RN_HK_STEP_BACK)) stepFrame(-1);
  if (on(RN_HK_SLOW)) toggleSlow();
  if (on(RN_HK_BOOKMARK) && session_) {
    uint64_t id = 0;
    if (rn_bookmark_add(session_, "", &id) == RN_OK) setNotice("Bookmark added");
  }
  if (on(RN_HK_SOFT_RESET)) pendingEvents_ |= RN_EV_SOFT_RESET;
  if (on(RN_HK_POWER_CYCLE)) pendingEvents_ |= RN_EV_POWER_CYCLE;
  if (on(RN_HK_TOGGLE_MODE)) toggleRecord();
  if (on(RN_HK_SAVE)) save();
  if (on(RN_HK_UNDO_TAKE) && session_) {
    if (rn_undo_take_switch(session_) == RN_OK) { resetFlash(); publishPicture(nullptr); setNotice("Back to previous take"); }
  }
}

bool EmuHost::stepOnce(Tick* t, bool audible) {
  uint8_t p1 = 0, p2 = 0;
  rn_input_sample_game(input_, rn_frame(session_), &p1, &p2);
  uint64_t before = rn_frame(session_);
  rn_step_info info{};
  if (rn_step(session_, p1, p2, pendingEvents_, &info) != RN_OK) {
    reportError("Emulation error");
    paused_ = true;
    return false;
  }
  pendingEvents_ = 0;
  if (info.end_of_take && info.frame == before) {
    paused_ = true;
    setNotice("End of the recording (paused)");
    return false;
  }
  emulatedFrames_ += 1;
  t->emulated = true;
  publishPicture(t);
  if (audible) {
    size_t n = 0;
    const int16_t* pcm = rn_audio(session_, &n);
    t->pcm = pcm;
    t->pcmCount = n;
    t->audible = n > 0;
  }
  return true;
}

void EmuHost::endFastForward() {
  bool wasActive = ff_.active;
  if (ff_.end(session_) != RN_OK) reportError("Couldn't return to record mode");
  if (wasActive) paused_ = pausedBeforeFF_ || rn_frame(session_) >= rn_take_length(session_);
  ffHeld_ = false;
  ffBlocked_ = false;
}

EmuHost::Tick EmuHost::tick(bool hold) {
  Tick t = tickInner(hold);
  t.newPicture = pictureDirty_;
  pictureDirty_ = false;
  return t;
}

EmuHost::Tick EmuHost::tickInner(bool hold) {
  Tick t;
  tickCount_ += 1;
  if (!session_) return t;
  uint32_t edges = 0, held = 0;
  rn_input_poll_hotkeys(input_, &edges, &held);
  if (hold) {
    if (ffHeld_) endFastForward();
    rewinding_ = false;
    rewindTicks_ = 0;
    return t;
  }
  if (edges) handleHotkeys(edges);

  bool practicing = rn_get_mode(session_) == RN_MODE_PRACTICE;
  bool wantRewind = (held & RN_HK_REWIND) != 0;
  bool wantFF = !wantRewind && !practicing && (held & RN_HK_FAST_FORWARD) != 0;
  if (wantFF && !ffHeld_) {
    ffHeld_ = true;
    pausedBeforeFF_ = paused_;
    ffBlocked_ = rn_frame(session_) >= rn_take_length(session_);
    if (ffBlocked_) setNotice(rn_take_length(session_) == 0 ? "Nothing recorded yet to fast-forward" : "End of the recording");
    else if (ff_.begin(session_) != RN_OK) { reportError("Couldn't fast-forward"); ffBlocked_ = true; }
  } else if (!wantFF && ffHeld_) {
    endFastForward();
  }

  if (wantRewind) {
    if (!rewinding_) resetFlash();
    rewinding_ = true;
    rewindTicks_ += 1;
    uint64_t n = rewindTicks_ > 240 ? 4 : rewindTicks_ > 90 ? 2 : 1;
    if (rn_frame(session_) > 0) {
      if (rn_rewind(session_, n) == RN_OK) publishPicture(&t);
      else reportError("Rewind failed");
    }
    return t;
  }
  if (rewinding_) { rewinding_ = false; rewindTicks_ = 0; }

  if (paused_) {
    int d = stepRepeater_.tick();
    if (d != 0) stepFrame(d);
  }

  if (ffHeld_) {
    if (ff_.active && !ffBlocked_) {
      bool atEnd = false;
      rn_status err = RN_OK;
      int n = ff_.step(session_, 4, &atEnd, &err);
      if (n > 0) publishPicture(&t);
      if (err != RN_OK) { ffBlocked_ = true; paused_ = true; reportError("Couldn't fast-forward"); }
      else if (atEnd) { ffBlocked_ = true; paused_ = true; setNotice("Reached the end of the recording (paused)"); }
    }
    return t;
  }

  int steps = 0;
  bool audible = false;
  if (paused_) {
    if (advanceRemaining_ > 0) { steps = 1; advanceRemaining_ -= 1; }
  } else {
    if (tickCount_ % uint64_t(slow_) == 0) steps = 1;
    audible = slow_ == SlowRate::normal;
  }
  int stepped = 0;
  for (int i = 0; i < steps; ++i)
    if (stepOnce(&t, audible)) stepped += 1;
  // A picture held back by the flash filter settles while nothing new is emulated.
  if (stepped == 0 && flashAltered_) publishPicture(&t);
  return t;
}

void EmuHost::housekeeping(double now) {
  if (!session_ || rewinding_) return;
  if (now - lastAutosave_ < 3.0) return;
  lastAutosave_ = now;
  if (rn_session_has_unsaved_changes(session_) && rn_session_autosave(session_) != RN_OK) reportError("Autosave failed");
}

}  // namespace rnl::interim
