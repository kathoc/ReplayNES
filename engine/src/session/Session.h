// Session: owns one core, the timeline (input DAG), checkpoints and bookmarks.
// Single-threaded: the caller must serialize all calls. Pacing (pause, slow motion, frame
// advance) is the frontend's job: it only decides WHEN to call step(); it never alters
// WHAT is recorded. Logical time is the integer frame index.
#pragma once
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "checkpoint/CheckpointStore.h"
#include "core/ICore.h"
#include "timeline/Timeline.h"

namespace rn {

class ProjectStore;

// Practice: non-recording play. Emulates frames with live input but writes nothing to the
// timeline/checkpoints; leaving practice restores the take exactly where it was.
enum class Mode : int { Record = 0, Replay = 1, Practice = 2 };

constexpr int kPracticeSlots = 8;

struct SessionOptions {
  CoreKind core = CoreKind::Nestopia;
  CheckpointPolicy checkpoints;
  bool dropCorruptStates = false;  // open(): discard corrupt state files instead of failing
  bool dropCorruptPractice = false;  // open(): discard corrupt practice slots instead of failing
};

struct StepInfo {
  uint64_t frame = 0;  // frame index after the call
  Mode mode = Mode::Record;
  bool branched = false;
  bool endOfTake = false;
  InputRecord applied;
  uint64_t take = 0;
};

struct Bookmark {
  uint64_t id = 0;
  std::string name;
  uint64_t frame = 0;
  uint64_t owner = 0;    // state owner segment (validity like checkpoints)
  uint64_t stateId = 0;  // 0 = no cached state
};

// A/B repeat slot (per project). A = full core state; B = length in frames after A.
struct PracticeSlot {
  bool used = false;  // has an A state
  std::string name;
  std::vector<uint8_t> state;  // core state envelope (compat id + frameIndex inside)
  std::string compat;
  uint32_t stateFormatVersion = 0;
  uint32_t crc = 0;           // crc32 of state
  uint64_t stateSeq = 0;      // sequence number at which A was captured (names the state file)
  bool hasB = false;
  uint64_t length = 0;        // B - A in frames (valid if hasB)
  bool hasTakeFrame = false;  // A was taken on the take (not inside a practice run)
  uint64_t takeFrame = 0, takeId = 0;
  uint64_t createdSeq = 0, updatedSeq = 0;
  // Runtime only: emulation-continuity anchor of A (epoch 0 = none, e.g. after reopening).
  uint64_t anchorEpoch = 0, anchorCount = 0;
  // Persistence bookkeeping.
  bool stateJournaled = false, stateFiled = false;
};

struct PracticeStatus {
  bool active = false;     // in Mode::Practice
  int anchorSlot = -1;     // slot of the live current anchor (-1: none / practice start)
  uint64_t counter = 0;    // frames since the current anchor (0 if continuity was broken)
  uint64_t returnFrame = 0;
  uint64_t rewindAvailable = 0;
};

struct TakeInfo {
  uint64_t id, parent, branchFrame, length, createdSeq;
  bool active;
  size_t children;
};

class Session {
 public:
  static Status create(std::vector<uint8_t> rom, const std::string& romPath, const SessionOptions& opt,
                       std::unique_ptr<Session>& out);
  ~Session();

  // --- play / record
  Mode mode() const { return mode_; }
  // Entering Practice keeps the current machine state; leaving it (Record/Replay) restores the
  // take state at the return frame (== frame(), which never moves during practice).
  Status setMode(Mode m);
  Status step(uint8_t p1, uint8_t p2, uint8_t events, StepInfo* info = nullptr);
  uint64_t frame() const { return frame_; }  // take cursor (the return frame while practicing)
  uint64_t takeLength() const { return tl_.length(); }
  Status seek(uint64_t frame);  // WrongMode in practice
  // Record/Replay: seek back on the take. Practice: rewind the practice run (bounded ring),
  // never before the current anchor (A or practice start).
  Status rewind(uint64_t n);

  const uint32_t* video() const { return core_->video(); }
  const uint16_t* videoCodes(uint32_t* burstPhase, uint64_t* frame) const { return core_->videoCodes(burstPhase, frame); }
  const int16_t* audio(size_t* count) const;
  uint64_t stateHash() { return core_->machineHash(); }
  uint64_t videoHash() const { return core_->videoHash(); }
  uint64_t audioHash() const { size_t n; const int16_t* a = audio(&n); return audioHashOf(a, n); }
  static uint64_t audioHashOf(const int16_t* a, size_t n);

  // --- bookmarks
  Status bookmarkAdd(const std::string& name, uint64_t* id);
  Status bookmarkRemove(uint64_t id);
  Status bookmarkRename(uint64_t id, const std::string& name);
  Status bookmarkGoto(uint64_t id);
  const std::vector<Bookmark>& bookmarks() const { return bookmarks_; }
  bool bookmarkOnActiveTake(const Bookmark& b) const { return tl_.stateValid(b.frame, b.owner); }

  // --- practice / A-B slots
  Status practiceSetA(int slot);
  Status practiceSetB(int slot);
  Status practiceGotoA(int slot);
  Status practiceRename(int slot, const std::string& name);
  Status practiceClear(int slot);
  const PracticeSlot* practiceSlot(int slot) const;
  bool practiceBSettable(int slot) const;
  PracticeStatus practiceStatus() const;
  uint64_t practiceCounter() const;
  uint32_t droppedPracticeSlots() const { return droppedPracticeMask_; }

  // --- takes
  std::vector<TakeInfo> takes() const;
  uint64_t activeTake() const { return tl_.head(); }
  Status activateTake(uint64_t id);
  Status undoTakeSwitch();
  size_t undoDepth() const { return tl_.undo().size(); }

  // --- persistence
  Status save();
  Status autosave();
  Status saveAs(const std::string& dir);
  bool hasProject() const { return store_ != nullptr; }
  bool recovered() const { return recovered_; }
  bool unsavedChanges() const { return changeSeq_ != savedSeq_; }
  std::string projectDir() const;

  // --- accessors
  const Timeline& timeline() const { return tl_; }
  const CheckpointStore& checkpoints() const { return cps_; }
  const std::vector<uint8_t>& rom() const { return rom_; }
  const std::string& romPath() const { return romPath_; }
  const std::string& romSha256() const { return romSha_; }
  CoreKind coreKind() const { return core_->kind(); }
  ICore& core() { return *core_; }
  const SessionOptions& options() const { return opt_; }

 private:
  friend class ProjectStore;
  Session() = default;
  void touch() { ++changeSeq_; }
  Status resync(uint64_t target, bool forceReload);
  Status powerOnFresh();
  void maybeCheckpoint();
  Status captureState(std::vector<uint8_t>& out) { return core_->saveState(out); }
  // Take state at the cursor (the saved return state while practicing).
  Status cursorState(std::vector<uint8_t>& out);
  Mode takeMode() const { return mode_ == Mode::Practice ? returnMode_ : mode_; }
  Status enterPractice();
  Status leavePractice(Mode to);
  void resetPracticeRun(std::vector<uint8_t> base);
  Status practiceRewind(uint64_t n);
  uint64_t practiceFloor() const;
  bool checkSlot(int slot, Status& err) const;

  SessionOptions opt_;
  std::unique_ptr<ICore> core_;
  std::vector<uint8_t> rom_;
  std::string romPath_, romSha_;
  Timeline tl_;
  CheckpointStore cps_;
  std::vector<Bookmark> bookmarks_;
  uint64_t nextBookmarkId_ = 1;
  Mode mode_ = Mode::Record;
  uint64_t frame_ = 0;
  bool audioValid_ = false;
  bool recovered_ = false;
  uint64_t changeSeq_ = 0, savedSeq_ = 0;
  std::unique_ptr<ProjectStore> store_;

  // Emulation continuity: epoch changes on every discontinuity (seek, state load, take switch,
  // leaving practice, goto A); count = frames emulated (practice rewind moves it back).
  uint64_t emuEpoch_ = 1, emuCount_ = 0;
  int curAnchorSlot_ = -1;  // anchor of the practice counter (-1: practice start / none)
  uint64_t curAnchorEpoch_ = 0, curAnchorCount_ = 0;
  // A/B slots (persisted with the project).
  std::array<PracticeSlot, kPracticeSlots> slots_;
  uint64_t practiceSeq_ = 0;
  uint32_t droppedPracticeMask_ = 0;
  // Practice run (memory only).
  Mode returnMode_ = Mode::Record;
  std::vector<uint8_t> returnState_;  // take state at frame_ when practice was entered
  uint64_t pEntryEpoch_ = 0, pEntryCount_ = 0;
  struct PracticeSnap {
    uint64_t count;
    std::vector<uint8_t> state;
  };
  std::deque<PracticeSnap> pSnaps_;      // every denseInterval frames, front = oldest
  std::vector<InputRecord> pInputs_;     // inputs applied since pSnaps_.front().count
};

}  // namespace rn
