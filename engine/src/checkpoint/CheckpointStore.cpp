#include "checkpoint/CheckpointStore.h"

#include <algorithm>

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
  // Only what is written to the project is checksummed, when it is written (a sparse state on the
  // StateWriter's thread). On the frame thread checksumming a large state took most of a recorded
  // frame (Sega System 32: 22 MB, 60 ms every 30 frames on a Mac).
  c.crc = kind == CpKind::Dense || kind == CpKind::Sparse ? 0 : crc32(data.data(), data.size());
  c.insertSeq = ++seq_;
  lastBytes_ = data.size();
  c.data = std::move(data);
  uint64_t id = c.id;
  byId_.emplace(id, std::move(c));
  if (kind == CpKind::Dense) {
    ++denseCount_;
    denseBytes_ += byId_.at(id).data.size();
    evictDense();
  }
  return id;
}

Status CheckpointStore::insertLoaded(Checkpoint c) {
  if (c.id == 0 || byId_.count(c.id)) return Error(Err::Corrupt, "duplicate checkpoint id");
  if (c.id >= nextId_) nextId_ = c.id + 1;
  c.insertSeq = ++seq_;
  if (c.kind == CpKind::Dense) {
    ++denseCount_;
    denseBytes_ += c.data.size();
  }
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
  dropped(it->second);
  if (it->second.writing) held_.push_back(std::move(it->second.data));  // the writer still reads it
  byId_.erase(it);
}

void CheckpointStore::dropped(Checkpoint& c) {
  if (c.kind != CpKind::Dense) return;
  --denseCount_;
  denseBytes_ -= std::min(denseBytes_, c.data.size());
  if (spare_.size() < 2 && !c.data.empty()) spare_.push_back(std::move(c.data));
}

void CheckpointStore::trimResident() {
  size_t resident = 0;
  for (auto& kv : byId_)
    if (kv.second.kind == CpKind::Sparse && kv.second.persisted && !kv.second.offloaded) resident += kv.second.data.size();
  while (resident > policy_.sparseResidentBytes) {
    Checkpoint* victim = nullptr;
    for (auto& kv : byId_) {
      Checkpoint& c = kv.second;
      if (c.kind == CpKind::Sparse && c.persisted && !c.offloaded && (!victim || c.insertSeq < victim->insertSeq)) victim = &c;
    }
    if (!victim) break;
    resident -= victim->data.size();
    std::vector<uint8_t>().swap(victim->data);
    victim->offloaded = true;
  }
}

void CheckpointStore::prepareSpare() {
  if (!spare_.empty() || !lastBytes_) return;
  spare_.emplace_back(lastBytes_);  // zero-filled: every page touched here, not in the next checkpoint
}

std::vector<uint8_t> CheckpointStore::takeSpare() {
  if (spare_.empty()) return {};
  std::vector<uint8_t> v = std::move(spare_.back());
  spare_.pop_back();
  return v;
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
  // Keeps the newest: by count and by bytes (but always the last one added).
  while (denseCount_ > 1 && (denseCount_ > policy_.denseCapacity || denseBytes_ > policy_.denseBytes)) {
    auto victim = byId_.end();
    for (auto it = byId_.begin(); it != byId_.end(); ++it)
      if (it->second.kind == CpKind::Dense && (victim == byId_.end() || it->second.insertSeq < victim->second.insertSeq))
        victim = it;
    if (victim == byId_.end()) break;
    dropped(victim->second);
    byId_.erase(victim);
  }
}

size_t CheckpointStore::memoryBytes() const {
  size_t n = 0;
  for (auto& kv : byId_) n += kv.second.data.size();
  return n;
}

}  // namespace rn
