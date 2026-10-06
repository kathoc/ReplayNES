// INTERIM FRONTEND LOGIC (Steam Deck plan, Step 2 skeleton).
// Session ownership and the per-frame transport decisions (pause, frame advance / step back,
// slow 1/2, rewind while held, fast-forward over the recorded take, record <-> replay, autosave),
// following EmulationController.tickFrame on macOS, directly on the engine C API. The temporary
// project lives in the session folder (current.nesrec) and is saved at quit. The shared frontend
// core (plan Step 3: playback/record-toggle logic, resume record + session lock) replaces this.
// Not thread-safe: called from the frame loop only (rn_input itself is internally locked).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "interim/playback_logic.h"
#include "replaynes/replaynes.h"

namespace rnl::interim {

class EmuHost {
 public:
  EmuHost();
  ~EmuHost();
  EmuHost(const EmuHost&) = delete;
  EmuHost& operator=(const EmuHost&) = delete;

  rn_input* input() const { return input_; }
  rn_session* session() const { return session_; }

  /// New recording of `rom` in the temporary project (`tempProject`). A previous temporary
  /// project with recorded content is kept: moved to `projectsDir` first.
  bool openRom(const std::string& rom, const std::string& tempProject, const std::string& projectsDir, std::string* error);
  /// Reopens the temporary project of the last run (at its saved cursor, paused).
  bool resume(const std::string& tempProject, std::string* error);
  /// Saves (if requested) and closes.
  void closeSession(bool save);

  struct Tick {
    bool newPicture = false;  // picture() changed
    bool audible = false;     // pcm/pcmCount hold this frame's audio for the speakers
    const int16_t* pcm = nullptr;
    size_t pcmCount = 0;
    bool emulated = false;    // a frame was emulated with live input (input sample -> picture)
  };
  /// One display frame. `hold` = UI menu open: hotkeys are polled, nothing is emulated.
  Tick tick(bool hold);
  /// After the present: autosave every few seconds when there is unsaved work.
  void housekeeping(double now);

  const uint32_t* picture() const { return display_.data(); }
  /// CRT side channel of picture() (display only): rn_video_indices of the same frame + whether
  /// the flash filter changed it. codes == nullptr when the core has none.
  struct Signal {
    const uint16_t* codes = nullptr;
    uint32_t burstPhase = 0;
    uint64_t ordinal = 0;
    bool flashAltered = false;
  };
  const Signal& signal() const { return signal_; }

  // Commands (UI / hotkeys).
  void togglePause();
  void setPaused(bool p);
  void stepFrame(int dir);
  void toggleSlow();
  void toggleRecord();
  void save();
  void setFlashLevel(rn_flash_level level);
  rn_flash_level flashLevel() const { return flashLevel_; }
  StepRepeater& stepRepeater() { return stepRepeater_; }

  // Status for the UI.
  bool paused() const { return paused_; }
  bool rewinding() const { return rewinding_; }
  bool fastForwarding() const { return ff_.active && !ffBlocked_; }
  SlowRate slow() const { return slow_; }
  std::string romName() const;
  const std::string& notice() const { return notice_; }
  double noticeTime() const { return noticeTime_; }
  void setNotice(const std::string& n);
  uint64_t emulatedFrames() const { return emulatedFrames_; }

 private:
  Tick tickInner(bool hold);
  void handleHotkeys(uint32_t edges);
  bool stepOnce(Tick* t, bool audible);
  void publishPicture(Tick* t);
  void resetFlash();
  void endFastForward();
  void reportError(const char* what);

  rn_session* session_ = nullptr;
  rn_input* input_ = nullptr;
  rn_flash_filter* flash_ = nullptr;
  rn_flash_level flashLevel_ = RN_FLASH_STANDARD;
  bool flashAltered_ = false;
  Signal signal_;
  bool pictureDirty_ = false;
  std::vector<uint32_t> display_;
  bool paused_ = false;
  bool rewinding_ = false;
  int rewindTicks_ = 0;
  int advanceRemaining_ = 0;
  SlowRate slow_ = SlowRate::normal;
  FastForward ff_;
  bool ffHeld_ = false;
  bool ffBlocked_ = false;
  bool pausedBeforeFF_ = false;
  StepRepeater stepRepeater_;
  uint8_t pendingEvents_ = 0;
  uint64_t tickCount_ = 0;
  uint64_t emulatedFrames_ = 0;
  double lastAutosave_ = 0;
  std::string notice_;
  double noticeTime_ = 0;
};

}  // namespace rnl::interim
