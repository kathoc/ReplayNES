#include "checkpoint/CheckpointStore.h"

#include "util/Hash.h"

namespace rn {

uint64_t CheckpointStore::add(uint64_t frame, uint64_t owner, CpKind kind, const std::string& compat, uint32_t sfv,
                              std::vector<uint8_t> data) {
  Checkpoint c;
  c.id = nextId_++;
  c.frame = frame;
  c.owner = owner;
  c.kind = kind;
  c.compat = compat;
  c.stateFormatVersion = sfv;
  c.crc = crc32(data.data(), data.size());
  c.insertSeq = ++seq_;
  c.data = std::move(data);
  uint64_t id = c.id;
  byId_.emplace(id, std::move(c));
  if (kind == CpKind::Dense) {
    ++denseCount_;
    evictDense();
  }
  return id;
}

Status CheckpointStore::insertLoaded(Checkpoint c) {
  if (c.id == 0 || byId_.count(c.id)) return Error(Err::Corrupt, "duplicate checkpoint id");
  if (c.id >= nextId_) nextId_ = c.id + 1;
  c.insertSeq = ++seq_;
  if (c.kind == CpKind::Dense) ++denseCount_;
  uint64_t id = c.id;
  byId_.emplace(id, std::move(c));
  return Status::Ok();
}

const Checkpoint* CheckpointStore::get(uint64_t id) const {
  auto it = byId_.find(id);
  return it == byId_.end() ? nullptr : &it->second;
}

Checkpoint* CheckpointStore::getMutable(uint64_t id) {
  auto it = byId_.find(id);
  return it == byId_.end() ? nullptr : &it->second;
}

void CheckpointStore::remove(uint64_t id) {
  auto it = byId_.find(id);
  if (it == byId_.end()) return;
  if (it->second.kind == CpKind::Dense) --denseCount_;
  byId_.erase(it);
}

const Checkpoint* CheckpointStore::find(uint64_t frame, uint64_t owner, int kindFilter) const {
  for (auto& kv : byId_) {
    const Checkpoint& c = kv.second;
    if (c.frame == frame && c.owner == owner && (kindFilter < 0 || int(c.kind) == kindFilter)) return &c;
  }
  return nullptr;
}

const Checkpoint* CheckpointStore::best(uint64_t target, const Timeline& tl) const {
  const Checkpoint* b = nullptr;
  for (auto& kv : byId_) {
    const Checkpoint& c = kv.second;
    if (c.frame > target) continue;
    if (b && c.frame <= b->frame) continue;
    if (!tl.stateValid(c.frame, c.owner)) continue;
    b = &c;
  }
  return b;
}

void CheckpointStore::evictDense() {
  while (denseCount_ > policy_.denseCapacity) {
    auto victim = byId_.end();
    for (auto it = byId_.begin(); it != byId_.end(); ++it)
      if (it->second.kind == CpKind::Dense && (victim == byId_.end() || it->second.insertSeq < victim->second.insertSeq))
        victim = it;
    if (victim == byId_.end()) break;
    byId_.erase(victim);
    --denseCount_;
  }
}

size_t CheckpointStore::memoryBytes() const {
  size_t n = 0;
  for (auto& kv : byId_) n += kv.second.data.size();
  return n;
}

}  // namespace rn
