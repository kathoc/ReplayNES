// Timeline: per-frame input log organised as a DAG of append-only segments.
//  * InputRecord = final NES bitfields actually given to the core + system event flags.
//  * Segment = {id, parent, startFrame, records}. Records are never modified once written;
//    a segment may only grow at its end. Branching creates a new child segment.
//  * A take is the path root..segment; the active head selects one take. On a path, each
//    segment covers [start, start of the next segment on the path) (last: its own end).
//  * Ownership of a machine state at frame f (= state after f frames) is the segment that
//    covers frame f-1 on the path it was captured on (0 for f == 0, valid for every path).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "util/Status.h"

namespace rn {

struct InputRecord {
  uint8_t p1 = 0, p2 = 0, events = 0;
  bool operator==(const InputRecord& o) const { return p1 == o.p1 && p2 == o.p2 && events == o.events; }
  bool operator!=(const InputRecord& o) const { return !(*this == o); }
};

struct Segment {
  uint64_t id = 0;
  uint64_t parent = 0;  // 0 = root
  uint64_t start = 0;
  uint64_t createdSeq = 0;
  std::vector<InputRecord> records;
  uint64_t end() const { return start + records.size(); }
  // Persistence bookkeeping (not part of identity).
  uint64_t fileCount = 0;      // records durable in segments/<id>.seg
  uint64_t journalCount = 0;   // records durable in file or journal
  bool journalCreated = false; // SEG_NEW durable (file or journal)
};

struct PathEntry {
  uint64_t seg;
  uint64_t start;
  uint64_t end;  // exclusive end on this path
};

struct UndoEntry {
  uint64_t head;
  uint64_t frame;
};

class Timeline {
 public:
  uint64_t head() const { return head_; }
  uint64_t length() const { return path_.empty() ? 0 : path_.back().end; }
  const std::vector<PathEntry>& path() const { return path_; }
  const std::map<uint64_t, Segment>& segments() const { return segs_; }
  std::map<uint64_t, Segment>& mutableSegments() { return segs_; }
  const Segment* segment(uint64_t id) const;
  uint64_t nextId() const { return nextId_; }
  uint64_t seq() const { return seq_; }

  // Record lookup on the active path (frame < length()).
  const InputRecord* recordAt(uint64_t frame) const;
  uint64_t segmentCovering(uint64_t frame) const;  // 0 if frame >= length
  uint64_t stateOwner(uint64_t frame) const { return frame == 0 ? 0 : segmentCovering(frame - 1); }
  bool stateValid(uint64_t frame, uint64_t owner) const;
  // Same checks against the path ending at an arbitrary segment.
  bool stateValidFor(uint64_t leaf, uint64_t frame, uint64_t owner) const;

  // Records `r` for frame f (f <= length()). Appends at the end or creates a branch.
  Status record(uint64_t frame, const InputRecord& r, bool& branched);
  Status setHead(uint64_t segId);  // 0 = empty timeline only allowed when no segments

  std::vector<InputRecord> flattenActive() const;  // copy of the active take
  std::vector<PathEntry> pathFor(uint64_t leaf) const;
  size_t childCount(uint64_t segId) const;

  // Undo of head switches ("前の試行へ戻す").
  std::vector<UndoEntry>& undo() { return undo_; }
  const std::vector<UndoEntry>& undo() const { return undo_; }

  // Loading (persistence) - bypasses recording rules but validates structure.
  Status insertLoaded(Segment s);
  void setCounters(uint64_t nextId, uint64_t seq) { nextId_ = nextId; seq_ = seq; }
  Status appendLoaded(uint64_t segId, uint64_t offset, const std::vector<InputRecord>& recs);
  Status validate() const;

 private:
  void rebuildPath();
  std::map<uint64_t, Segment> segs_;
  std::vector<PathEntry> path_;
  std::vector<UndoEntry> undo_;
  uint64_t head_ = 0;
  uint64_t nextId_ = 1;
  uint64_t seq_ = 0;
};

// Run-length encoding used by segment files and journal records:
//   repeated { varint runLength>=1, u8 p1, u8 p2, u8 events }
void encodeRecords(const InputRecord* recs, size_t n, std::vector<uint8_t>& out);
bool decodeRecords(const uint8_t* data, size_t n, uint64_t expectCount, std::vector<InputRecord>& out);

}  // namespace rn
