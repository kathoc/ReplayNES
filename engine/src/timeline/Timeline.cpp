#include "timeline/Timeline.h"

#include <algorithm>
#include <cstddef>

#include "util/Bytes.h"

namespace rn {

const Segment* Timeline::segment(uint64_t id) const {
  auto it = segs_.find(id);
  return it == segs_.end() ? nullptr : &it->second;
}

std::vector<PathEntry> Timeline::pathFor(uint64_t leaf) const {
  std::vector<uint64_t> chain;
  for (uint64_t id = leaf; id != 0;) {
    const Segment* s = segment(id);
    if (!s || chain.size() > segs_.size()) return {};
    chain.push_back(id);
    id = s->parent;
  }
  std::reverse(chain.begin(), chain.end());
  std::vector<PathEntry> p;
  for (size_t i = 0; i < chain.size(); ++i) {
    const Segment& s = segs_.at(chain[i]);
    uint64_t end = (i + 1 < chain.size()) ? segs_.at(chain[i + 1]).start : s.end();
    p.push_back({s.id, s.start, end});
  }
  return p;
}

void Timeline::rebuildPath() { path_ = pathFor(head_); }

static const PathEntry* findEntry(const std::vector<PathEntry>& p, uint64_t frame) {
  // Last entry whose start <= frame.
  auto it = std::upper_bound(p.begin(), p.end(), frame, [](uint64_t f, const PathEntry& e) { return f < e.start; });
  if (it == p.begin()) return nullptr;
  --it;
  return frame < it->end ? &*it : nullptr;
}

const InputRecord* Timeline::recordAt(uint64_t frame) const {
  const PathEntry* e = findEntry(path_, frame);
  if (!e) return nullptr;
  const Segment& s = segs_.at(e->seg);
  return &s.records[size_t(frame - s.start)];
}

uint64_t Timeline::segmentCovering(uint64_t frame) const {
  const PathEntry* e = findEntry(path_, frame);
  return e ? e->seg : 0;
}

static bool validOn(const std::vector<PathEntry>& p, uint64_t frame, uint64_t owner) {
  if (owner == 0) return frame == 0;
  for (auto& e : p)
    if (e.seg == owner) return frame > e.start && frame <= e.end;
  return false;
}

bool Timeline::stateValid(uint64_t frame, uint64_t owner) const { return validOn(path_, frame, owner); }

bool Timeline::stateValidFor(uint64_t leaf, uint64_t frame, uint64_t owner) const {
  return validOn(pathFor(leaf), frame, owner);
}

Status Timeline::record(uint64_t frame, const InputRecord& r, bool& branched) {
  branched = false;
  uint64_t len = length();
  if (frame > len) return Error(Err::OutOfRange, "record beyond take end");
  if (head_ != 0 && frame == len) {
    segs_.at(head_).records.push_back(r);
    path_.back().end++;
    return Status::Ok();
  }
  // New segment: either the very first one, or a branch at frame < len.
  Segment s;
  s.id = nextId_++;
  s.parent = stateOwner(frame);
  s.start = frame;
  s.createdSeq = ++seq_;
  s.records.push_back(r);
  uint64_t id = s.id;
  segs_.emplace(id, std::move(s));
  if (head_ != 0) {
    undo_.push_back({head_, frame});
    branched = true;
  }
  head_ = id;
  rebuildPath();
  return Status::Ok();
}

Status Timeline::setHead(uint64_t segId) {
  if (segId == 0) {
    if (!segs_.empty()) return Error(Err::InvalidArg, "take 0 does not exist");
    head_ = 0;
    path_.clear();
    return Status::Ok();
  }
  if (!segment(segId)) return Error(Err::NotFound, "take " + std::to_string(segId) + " does not exist");
  head_ = segId;
  rebuildPath();
  return Status::Ok();
}

std::vector<InputRecord> Timeline::flattenActive() const {
  std::vector<InputRecord> out;
  out.reserve(size_t(length()));
  for (auto& e : path_) {
    const Segment& s = segs_.at(e.seg);
    out.insert(out.end(), s.records.begin() + ptrdiff_t(e.start - s.start), s.records.begin() + ptrdiff_t(e.end - s.start));
  }
  return out;
}

size_t Timeline::childCount(uint64_t segId) const {
  size_t n = 0;
  for (auto& kv : segs_) n += kv.second.parent == segId ? 1 : 0;
  return n;
}

Status Timeline::insertLoaded(Segment s) {
  if (s.id == 0 || segs_.count(s.id)) return Error(Err::Corrupt, "duplicate or zero segment id");
  if (s.id >= nextId_) nextId_ = s.id + 1;
  if (s.createdSeq > seq_) seq_ = s.createdSeq;
  uint64_t id = s.id;
  segs_.emplace(id, std::move(s));
  return Status::Ok();
}

Status Timeline::appendLoaded(uint64_t segId, uint64_t offset, const std::vector<InputRecord>& recs) {
  auto it = segs_.find(segId);
  if (it == segs_.end()) return Error(Err::Corrupt, "journal appends to unknown segment " + std::to_string(segId));
  Segment& s = it->second;
  if (offset != s.records.size()) {
    if (offset + recs.size() <= s.records.size()) return Status::Ok();  // already contained (idempotent)
    if (offset > s.records.size())
      return Error(Err::Corrupt, "journal gap in segment " + std::to_string(segId));
    // Partial overlap: verify the overlapping part matches, then append the rest.
    size_t overlap = size_t(s.records.size() - offset);
    for (size_t i = 0; i < overlap; ++i)
      if (s.records[size_t(offset) + i] != recs[i]) return Error(Err::Corrupt, "journal conflicts with segment");
    s.records.insert(s.records.end(), recs.begin() + ptrdiff_t(overlap), recs.end());
  } else {
    s.records.insert(s.records.end(), recs.begin(), recs.end());
  }
  if (segId == head_) rebuildPath();
  return Status::Ok();
}

Status Timeline::validate() const {
  for (auto& kv : segs_) {
    const Segment& s = kv.second;
    if (s.parent != 0) {
      const Segment* p = segment(s.parent);
      if (!p) return Error(Err::Corrupt, "segment " + std::to_string(s.id) + " has missing parent");
      if (p->id >= s.id) return Error(Err::Corrupt, "segment parent order invalid");
      if (s.start <= p->start) return Error(Err::Corrupt, "segment starts before its parent");
    } else if (s.start != 0) {
      return Error(Err::Corrupt, "root segment must start at frame 0");
    }
  }
  if (head_ != 0 && !segment(head_)) return Error(Err::Corrupt, "active head missing");
  if (head_ == 0 && !segs_.empty()) return Error(Err::Corrupt, "no active head");
  return Status::Ok();
}

void encodeRecords(const InputRecord* recs, size_t n, std::vector<uint8_t>& out) {
  ByteWriter w;
  size_t i = 0;
  while (i < n) {
    size_t j = i + 1;
    while (j < n && recs[j] == recs[i]) ++j;
    w.varint(j - i);
    w.u8(recs[i].p1);
    w.u8(recs[i].p2);
    w.u8(recs[i].events);
    i = j;
  }
  out.insert(out.end(), w.buf.begin(), w.buf.end());
}

bool decodeRecords(const uint8_t* data, size_t n, uint64_t expectCount, std::vector<InputRecord>& out) {
  ByteReader r(data, n);
  out.clear();
  while (r.ok() && r.remaining() > 0) {
    uint64_t run = r.varint();
    InputRecord rec;
    rec.p1 = r.u8();
    rec.p2 = r.u8();
    rec.events = r.u8();
    if (!r.ok() || run == 0 || out.size() + run > expectCount) return false;
    if (rec.events & ~0x03) return false;  // unknown event bits in inputFormatVersion 1
    out.insert(out.end(), size_t(run), rec);
  }
  return r.ok() && out.size() == expectCount;
}

}  // namespace rn
