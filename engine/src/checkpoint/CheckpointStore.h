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
  uint32_t crc = 0;
  uint64_t insertSeq = 0;
  bool persisted = false;
  std::vector<uint8_t> data;
};

struct CheckpointPolicy {
  uint32_t denseInterval = 30;
  uint32_t denseCapacity = 1200;  // ~10 min of rewind at 30f spacing
  uint32_t sparseInterval = 600;
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

 private:
  void evictDense();
  CheckpointPolicy policy_;
  std::map<uint64_t, Checkpoint> byId_;
  uint64_t nextId_ = 1;
  uint64_t seq_ = 0;
  size_t denseCount_ = 0;
};

}  // namespace rn
