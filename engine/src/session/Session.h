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
enum class Mode : int { Record = 0, Replay = 1, Practice = 2, Live = 3 };

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
  Status step(uint8_t p1, uint8_t p2, uint8_t events, StepInfo* info = nullptr);  // NES pads
  // One frame of any core's input: bytes beyond inputLayout().recordBytes must be zero.
  Status stepInput(const InputRecord& in, StepInfo* info = nullptr);
  const InputLayout& inputLayout() const { return layout_; }
  uint64_t frame() const { return frame_; }  // take cursor (the return frame while practicing)
  uint64_t takeLength() const { return tl_.length(); }
  Status seek(uint64_t frame);  // WrongMode in practice
  // Record/Replay: seek back on the take. Practice: rewind the practice run (bounded ring),
  // never before the current anchor (A or practice start).
  Status rewind(uint64_t n);
  // Optional paused/idle maintenance. No presentation, cursor or continuity changes.
  // At most budgetFrames core steps; Practice/Live and budget zero are no-ops.
  Status idleWork(uint32_t budgetFrames);

  const uint32_t* video() const { return core_->video(); }
  MediaInfo mediaInfo() const { return core_->mediaInfo(); }
  VideoFrame videoFrame() const { return core_->videoFrame(); }
  uint64_t samplesBefore(uint64_t frame) const { return core_->samplesBefore(frame); }
  const uint16_t* videoCodes(uint32_t* burstPhase, uint64_t* frame) const { return core_->videoCodes(burstPhase, frame); }
  const int16_t* audio(size_t* count) const;
  uint64_t stateHash() { return core_->machineHash(); }
  uint64_t videoHash() const { return core_->videoHash(); }
  uint64_t audioHash() const { size_t n; const int16_t* a = audio(&n); return audioHashOf(a, n, core_->mediaInfo().audioChannels); }
  static uint64_t audioHashOf(const int16_t* a, size_t n, uint32_t channels = 1);

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
  // Defines slot = [a, b) directly from take frames (cursor positions on the active take):
  // A = take state at frame a, length = b - a. The cursor, machine state, video buffer and
  // emulation continuity are restored exactly afterwards (rn_audio reports 0 samples, like a
  // seek). WrongMode in practice; InvalidArg unless a < b; OutOfRange if b > take length.
  Status practiceSetRange(int slot, uint64_t a, uint64_t b);
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
  uint64_t activeTake() const { return mode_ == Mode::Live ? 0 : tl_.head(); }
  Status activateTake(uint64_t id);
  Status undoTakeSwitch();
  size_t undoDepth() const { return tl_.undo().size(); }
  // Drops an abandoned take (a retry the user threw away): not the active take, no takes branching
  // from it, no bookmark or A/B slot on it. Its checkpoints and undo entries go too. Persisted by
  // the next full save (until then a crash may bring it back as an extra take; nothing else).
  Status discardTake(uint64_t id);
  // Cuts the active take at the cursor when every frame after it belongs to the active take's own
  // segment (branch frame < cursor < length) and nothing else hangs off them (no child take, no
  // bookmark / A/B slot on the take): playing on then continues the take instead of branching. The
  // take gets a new id; the earlier frames, the cursor and the machine state are unchanged.
  // WrongMode in practice; Ok without a change at the take end.
  Status truncateTake();

  // --- persistence
  Status save();
  Status autosave();
  Status saveAs(const std::string& dir);
  // "Reset Project": the project starts over from power-on (frame 0, record mode, empty
  // timeline: every take, bookmark, checkpoint and the take undo history are dropped). Same ROM
  // and project directory. keepPracticeSlots keeps the A/B slots (their A states stay valid; their
  // take position is cleared). A project is fully saved right away; segment / state ids are never
  // reused, so a crash at any point leaves either the old or the new project on disk. If the save
  // fails before its commit point the session is restored to its previous state. WrongMode in
  // practice.
  Status reset(bool keepPracticeSlots);
  bool hasProject() const { return store_ != nullptr; }
  bool recovered() const { return recovered_; }
  bool unsavedChanges() const { return mode_ != Mode::Live && changeSeq_ != savedSeq_; }
  std::string projectDir() const;

  // --- accessors
  const Timeline& timeline() const { return tl_; }
  const CheckpointStore& checkpoints() const { return cps_; }
  const std::vector<uint8_t>& rom() const { return rom_; }
  const std::string& romPath() const { return romPath_; }
  const std::string& romSha256() const { return romSha_; }
  CoreKind coreKind() const { return core_->kind(); }
  ICore& core() { clearRewindCache(); return *core_; }
  const SessionOptions& options() const { return opt_; }

 private:
  friend class ProjectStore;
  Session() = default;
  void touch() { ++changeSeq_; }
  // Pre-frame states, collected only during rewind; at most two dense-sized ranges.
  // No media is cached: the last frame is always rendered by the core.
  struct RewindBlock {
    uint64_t first = 0;
    std::vector<std::vector<uint8_t>> states;
  };
  std::deque<RewindBlock> rewindBlocks_;
  struct RewindPrefetch {
    RewindBlock block;
    uint64_t next = 0, end = 0;  // end == 0: no pending work
    bool fresh = false;
    std::vector<uint8_t> resume;
  } rewindPrefetch_;
  void clearRewindCache();
  uint64_t advanceRewindPrefetch(uint64_t budget);
  uint64_t prefetchOlderRewindBlock(uint64_t target, uint64_t budget = 5, uint64_t rewound = 0);
  uint64_t finishRewindPrefetch(uint64_t target);
  const std::vector<uint8_t>* rewindState(uint64_t frame) const;
  void cacheRewindState(RewindBlock& block, uint64_t frame);
  void retainRewindBlock(RewindBlock block);
  Status resync(uint64_t target, bool forceReload, RewindBlock* block = nullptr);
  Status powerOnFresh();
  void maybeCheckpoint(uint64_t frame, ICore* source = nullptr);
  // One independent logical machine, not another per-frame state cache. Shared
  // physical cores arbitrate instances exactly as they do for OfflineRenderer.
  std::unique_ptr<ICore> warmCore_;
  uint64_t warmNext_ = 0, warmEnd_ = 0;
  bool idleWarmComplete_ = false;
  // A fully checkpointed search window can be reused until checkpoint insertion
  // (which can evict dense entries) or any ordinary cache-invalidating operation.
  bool warmChecked_ = false;
  uint64_t warmCheckedFloor_ = 0, warmCheckedTop_ = 0, warmCheckedId_ = 0;
  Status warmRewindGap(uint64_t target, uint32_t budgetFrames, bool idle);
  Status captureState(std::vector<uint8_t>& out) { return core_->saveState(out); }
  // Take state at the cursor (the saved return state while practicing).
  Status cursorState(std::vector<uint8_t>& out);
  // Take state at frame f (not in practice); moves the core and frame_, renders no video and
  // does not touch emuEpoch_ (the caller restores the cursor).
  Status takeStateAt(uint64_t f, std::vector<uint8_t>& out);
  Mode takeMode() const { return mode_ == Mode::Practice ? returnMode_ : mode_; }
  Status enterPractice();
  Status leavePractice(Mode to);
  void resetPracticeRun(std::vector<uint8_t> base);
  Status practiceRewind(uint64_t n);
  uint64_t practiceFloor() const;
  bool checkSlot(int slot, Status& err) const;

  SessionOptions opt_;
  std::unique_ptr<ICore> core_;
  InputLayout layout_ = InputLayout::nes();  // core_->inputLayout() of the loaded content
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
