// The owner of the rn_session on the frame-loop thread: what one display frame emulates
// (pause, slow 1/2, frame advance / step back, rewind and fast-forward while held, practice A/B
// loop, record <-> replay), the flash filter on the shown picture, autosave and the status the UI
// shows. Port of the macOS EmulationController (apps/macos/Sources/App/EmulationController.swift)
// on the shared frontend core's state machines (rnf_record_toggle, rnf_step_repeater,
// rnf_fast_forward, rnf_practice_loop, rnf_frame_history, rnf_audio_fade_tail).
// The display loop decides only WHEN tick() runs; what is emulated is fully determined by the
// recorded input / event stream. No SDL here (unit-tested headless with the engine's test ROM).
// Not thread-safe: the frame loop thread only (rn_input itself is internally locked).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "audio_sink.h"
#include "replaynes/frontend.h"
#include "replaynes/replaynes.h"

namespace rnl {

/// UI-facing snapshot (EmuStatus on macOS).
struct EmuStatus {
  bool hasSession = false;
  uint64_t frame = 0;
  uint64_t takeLength = 0;
  bool recording = true;
  bool paused = true;
  int slow = 1;  // 1 normal, 2 = 1/2
  bool rewinding = false;
  bool fastForward = false;
  bool endOfTake = false;
  uint64_t activeTake = 0;
  size_t takeCount = 0;
  size_t undoDepth = 0;
  bool unsaved = false;
  std::string projectPath;
  std::string romPath;
  int advancePending = 0;
  bool flashActive = false;
  // Practice (frame / takeLength above stay the take's, frozen while practicing).
  bool practicing = false;
  int practiceSlot = -1;
  uint64_t practiceFrame = 0;
  uint64_t practiceLength = 0;
  bool practiceLooping = false;
  int practiceLoops = 0;
};

struct BookmarkInfo {
  uint64_t id = 0, frame = 0, takeId = 0;
  std::string name;
  bool onActiveTake = false;
};

struct TakeInfo {
  uint64_t id = 0, parentId = 0, branchFrame = 0, length = 0, createdSeq = 0;
  bool isActive = false;
  uint32_t childCount = 0;
};

struct SlotInfo {
  int index = 0;
  bool hasA = false, hasB = false;
  uint64_t length = 0;
  std::string name;
  bool hasTakeFrame = false;
  uint64_t takeFrame = 0, takeId = 0;
  std::string displayName() const;  // name, or "Section N"
};

struct SessionStructure {
  std::vector<BookmarkInfo> bookmarks;
  std::vector<TakeInfo> takes;
  std::vector<SlotInfo> slots;
  bool hasContent() const;  // takes with frames, bookmarks or A/B slots
};

/// Thumbnail capture hooks (ThumbnailManager; none in tests).
class FrameObserver {
 public:
  virtual ~FrameObserver() = default;
  /// A session was installed / replaced / reset (nullptr: none).
  virtual void sessionInstalled(rn_session* s) = 0;
  /// The take frame `frame` is what rn_video shows (after a step / seek), on the frame thread.
  virtual void frameShown(rn_session* s, uint64_t frame) = 0;
};

class EmulationController {
 public:
  EmulationController(rn_input* input, AudioSink* audio);
  ~EmulationController();
  EmulationController(const EmulationController&) = delete;
  EmulationController& operator=(const EmulationController&) = delete;

  // Hooks (all called on the frame thread).
  std::function<void(const std::string&)> onNotice;
  std::function<void(const std::string& title, const std::string& message)> onError;
  std::function<void(bool paused)> onPausedChanged;
  /// Count of physical presses so far (the "paused, press R to resume" hint).
  std::function<uint64_t()> pressSequence;
  FrameObserver* observer = nullptr;

  // Preferences.
  bool pauseAfterRewind = true;
  double autosaveInterval = 2.0;
  /// The session is the temporary project: while paused it is also fully saved now and then.
  bool tempSession = false;
  void setFlashLevel(rn_flash_level level);
  rn_flash_level flashLevel() const { return flashLevel_; }

  /// Takes ownership. A fresh recording (empty take at frame 0) runs at once, an opened project
  /// stays paused at its cursor. nullptr closes the current one (without saving).
  void install(rn_session* s);
  /// Closes the session (no save).
  void closeSession();
  rn_session* session() const { return session_; }

  struct Tick {
    bool newPicture = false;  // picture() changed
    bool emulated = false;    // a frame was emulated with live input (input sample -> picture)
  };
  /// One display frame: hotkeys, rewind / fast-forward holds, practice loop, emulation.
  Tick tick();
  /// After the present, with `slack` seconds before the next frame's work: live thumbnail,
  /// autosave.
  void afterFrame(double slack);

  const uint32_t* picture() const { return display_.data(); }
  uint64_t emulatedFrames() const { return emulatedFrames_; }

  // ---- commands (UI, hotkeys) ----
  void togglePause();
  void setPaused(bool p);
  bool paused() const { return paused_; }
  void frameAdvance(int n = 1);
  void stepBack(uint64_t n = 1);
  /// Paused D-pad stepping (InputRouter): step now, repeat while held.
  void pausedStep(int dir, bool down);
  void toggleSlow();
  void toggleRecord();
  void setRecording(bool rec);
  void rerecordHere();
  void seek(uint64_t frame);
  /// Timeline drag: paused, silent.
  void scrub(uint64_t frame);
  void jumpSeconds(double seconds);
  void setRewindHeld(bool h) { uiRewindHeld_ = h; }
  void setFastForwardHeld(bool h) { uiFastForwardHeld_ = h; }
  void requestEvent(uint8_t ev);
  void addBookmark(const std::string& name = "");
  void gotoBookmark(uint64_t id);
  void removeBookmark(uint64_t id);
  void renameBookmark(uint64_t id, const std::string& name);
  void activateTake(uint64_t id);
  void undoTake();
  /// Full save of a project with a folder. Returns false (and reports) on failure.
  bool save();
  /// Quit / background: full save for the temporary project, the journal for a project.
  /// Returns an error message ("" on success).
  std::string flushForResume(bool fullSave);
  /// After install(): back to the recorded take mode / position (paused).
  void applyResume(const rnf_resume_record& r);

  // Practice (A/B repeat).
  void startPractice(int slot);
  void stopPractice();
  void practiceSetA(int slot);
  void practiceSetB(int slot);
  void practiceRename(int slot, const std::string& name);
  void practiceClear(int slot);
  void practiceSetRange(int slot, uint64_t a, uint64_t b);
  void timelineMarkA(int slot);
  void timelineMarkB(int slot);

  /// "Reset Project": backup (a callback copying the project away; empty = none) then
  /// rn_session_reset. Returns an error message, "" when the project was reset.
  std::string resetProject(bool keepPracticeSlots, const std::function<std::string(const std::string& dir)>& backup);

  /// Current state (refreshed on every call: commands take effect at once).
  const EmuStatus& status() {
    updateStatus();
    return status_;
  }
  /// Bookmarks / takes / slots (rebuilt when changed, at most ~1/s while recording).
  const SessionStructure& structure();
  uint64_t structureVersion() const { return structureVersion_; }
  /// The visible A/B ranges of the active take.
  std::vector<rnf_timeline_range> visibleRanges();

  static const char* practiceBlockedText();

 private:
  void publishVideo(bool continuous = false);
  void show(const uint32_t* v);
  void resetFlashFilter();
  void seekCommand(uint64_t f);
  bool stepOnce(bool audible);
  void stepFrame(int dir);
  void handleHotkeys(uint32_t edges);
  void beginFastForward();
  void endFastForward();
  void tickFastForward();
  void tickPractice();
  void refreshPracticeLength(int slot);
  bool practiceSetBFromTake(int slot);
  bool timelineRange(int slot, rnf_timeline_range* out);
  void maybeAutosave(double slack);
  void updateStatus();
  void notice(const std::string& s);
  void reportError(const std::string& title);
  void markStructureDirty() { structureDirty_ = true; }
  void setMuted(bool m);

  rn_session* session_ = nullptr;
  rn_input* input_;
  AudioSink* audio_;
  rn_flash_filter* flash_ = nullptr;
  rn_flash_level flashLevel_ = RN_FLASH_STANDARD;
  std::vector<uint32_t> display_;
  bool pictureDirty_ = false;
  bool flashAltered_ = false;
  uint64_t lastFlashTick_ = 0;
  bool flashActiveShown_ = false;

  bool paused_ = true;
  int slow_ = 1;
  int advanceRemaining_ = 0;
  bool uiRewindHeld_ = false, uiFastForwardHeld_ = false;
  uint8_t pendingEvents_ = 0;
  bool rewinding_ = false, fastForward_ = false;
  int rewindTicks_ = 0;
  uint64_t tickCount_ = 0;
  bool endOfTake_ = false;
  uint64_t lastPressSeq_ = 0;
  bool pauseHintShown_ = false;
  // autosave
  double lastAutosave_ = 0, lastFullSave_ = 0;
  bool autosaveFailed_ = false, autosaveSoon_ = false;
  // fast-forward
  rnf_fast_forward* ff_ = nullptr;
  bool ffBlocked_ = false, pausedBeforeFF_ = true;
  rnf_step_repeater* stepRepeater_ = nullptr;
  // practice
  int practiceSlot_ = -1;
  uint64_t practiceLength_ = 0;
  rnf_practice_loop* practiceLoop_ = nullptr;
  uint64_t practiceSeq_ = uint64_t(1) << 40;  // input sampling clock while practicing (turbo phase)
  rn_mode modeBeforePractice_ = RN_MODE_RECORD;
  rnf_frame_history* history_ = nullptr;
  bool practiceRewindStarted_ = false;
  // thumbnails
  bool pendingThumb_ = false;
  uint64_t pendingThumbFrame_ = 0;
  uint64_t emulatedFrames_ = 0;
  // status / structure
  EmuStatus status_;
  SessionStructure structure_;
  bool structureDirty_ = true;
  uint64_t structureTick_ = 0;
  uint64_t structureVersion_ = 0;
  uint64_t lastStructureTake_ = ~uint64_t(0);
};

}  // namespace rnl
