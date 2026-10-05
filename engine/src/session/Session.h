// Session: owns one core, the timeline (input DAG), checkpoints and bookmarks.
// Single-threaded: the caller must serialize all calls. Pacing (pause, slow motion, frame
// advance) is the frontend's job: it only decides WHEN to call step(); it never alters
// WHAT is recorded. Logical time is the integer frame index.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "checkpoint/CheckpointStore.h"
#include "core/ICore.h"
#include "timeline/Timeline.h"

namespace rn {

class ProjectStore;

enum class Mode : int { Record = 0, Replay = 1 };

struct SessionOptions {
  CoreKind core = CoreKind::Nestopia;
  CheckpointPolicy checkpoints;
  bool dropCorruptStates = false;  // open(): discard corrupt state files instead of failing
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
  void setMode(Mode m) { if (m != mode_) { mode_ = m; touch(); } }
  Status step(uint8_t p1, uint8_t p2, uint8_t events, StepInfo* info = nullptr);
  uint64_t frame() const { return frame_; }
  uint64_t takeLength() const { return tl_.length(); }
  Status seek(uint64_t frame);
  Status rewind(uint64_t n) { return seek(frame_ - (n > frame_ ? frame_ : n)); }

  const uint32_t* video() const { return core_->video(); }
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
};

}  // namespace rn
