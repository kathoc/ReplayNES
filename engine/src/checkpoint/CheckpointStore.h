// CheckpointStore: machine-state snapshots used to accelerate seek. States are a cache, never
// the source of truth (the input log is). Each checkpoint carries the metadata needed to decide
// whether it is valid for the active take (see Timeline ownership rules).
//   Dense    - every denseInterval frames, bounded ring (oldest evicted), memory only.
//   Sparse   - every sparseInterval frames, permanent, persisted.
//   Bookmark - pinned by a bookmark, persisted.
//   Head     - state at the cursor written on save, for exact resume, persisted.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "timeline/Timeline.h"

namespace rn {

enum class CpKind : uint8_t { Dense = 0, Sparse = 1, Bookmark = 2, Head = 3 };

struct Checkpoint {
  uint64_t id = 0;
  uint64_t frame = 0;
  uint64_t owner = 0;  // segment owning the state (Timeline::stateOwner)
  CpKind kind = CpKind::Dense;
  std::string compat;
  uint32_t stateFormatVersion = 0;
  uint32_t crc = 0;  // of data, for what is persisted (set when its file is written; Dense: 0)
  uint64_t insertSeq = 0;
  bool persisted = false;
  // A persisted sparse checkpoint whose bytes left memory (CheckpointStore::trimResident): `data` is empty,
  // Session::checkpointData reads them back from the project.
  bool offloaded = false;
  // A sparse state on its way to its file (StateWriter): `data` is read there meanwhile and stays as is.
  bool writing = false;
  // Its file is written and durable, its reference (journal / index) is not yet (then: persisted).
  bool fileWritten = false;
  std::vector<uint8_t> data;
};

struct CheckpointPolicy {
  uint32_t denseInterval = 30;
  uint32_t denseCapacity = 1200;  // ~10 min of rewind at 30f spacing
  uint32_t sparseInterval = 600;
  // Bytes the dense checkpoints may hold (oldest dropped first, as by count). Without it a large state
  // filled memory: 1200 x 22 MB (Sega System 32) = 26 GB; at 1 GiB that game keeps ~45 (23 s), and
  // rewinding further back starts from the sparse checkpoints.
  size_t denseBytes = size_t(1) << 30;
  // Bytes of persisted sparse checkpoints kept in memory (the oldest leave first; read back when used).
  // Every sparse checkpoint was kept: a 22 MB state is 1.3 GB per 10 minutes of take, and opening a
  // project read all of them.
  size_t sparseResidentBytes = size_t(512) << 20;
};

class CheckpointStore {
 public:
  explicit CheckpointStore(CheckpointPolicy p = {}) : policy_(p) {}
  const CheckpointPolicy& policy() const { return policy_; }

  uint64_t add(uint64_t frame, uint64_t owner, CpKind kind, const std::string& compat, uint32_t sfv,
               std::vector<uint8_t> data);
  Status insertLoaded(Checkpoint c);
  const Checkpoint* get(uint64_t id) const;
  Checkpoint* getMutable(uint64_t id);
  void remove(uint64_t id);
  // Exact (frame, owner) match of a kind (or any kind if kindFilter < 0).
  const Checkpoint* find(uint64_t frame, uint64_t owner, int kindFilter = -1) const;
  // Best valid checkpoint with frame <= target for the active path.
  const Checkpoint* best(uint64_t target, const Timeline& tl) const;
  const std::map<uint64_t, Checkpoint>& all() const { return byId_; }
  uint64_t nextId() const { return nextId_; }
  void setNextId(uint64_t n) { if (n > nextId_) nextId_ = n; }
  size_t memoryBytes() const;
  /// A buffer of a dropped dense checkpoint, for the next one (empty if none): a large state written
  /// into fresh memory paid its zero fill and page faults on every checkpoint.
  std::vector<uint8_t> takeSpare();
  /// A spare of the newest checkpoint's size, its memory touched now (Session::checkpointWork, in the
  /// slack), when there is none: until dense checkpoints are dropped every one needed fresh memory.
  void prepareSpare();
  /// Persisted sparse checkpoints beyond sparseResidentBytes leave memory (oldest first).
  void trimResident();
  /// The bytes of removed checkpoints that were being written: freed once no write runs.
  void releaseHeld() { held_.clear(); }

 private:
  void evictDense();
  void dropped(Checkpoint& c);  // the bookkeeping of a checkpoint leaving the store
  CheckpointPolicy policy_;
  std::map<uint64_t, Checkpoint> byId_;
  uint64_t nextId_ = 1;
  uint64_t seq_ = 0;
  size_t denseCount_ = 0;
  size_t denseBytes_ = 0;
  size_t lastBytes_ = 0;  // the newest checkpoint's size
  std::vector<std::vector<uint8_t>> spare_;  // at most 2
  std::vector<std::vector<uint8_t>> held_;   // removed while writing (Checkpoint::writing)
};

}  // namespace rn
