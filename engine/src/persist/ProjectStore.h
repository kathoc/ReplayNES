// ProjectStore: the .nesrec package (a directory). See docs/FILE_FORMAT.md.
//   manifest.json            identity + compatibility, committed LAST (generation g)
//   timeline/index.json      segments, checkpoints, head, cursor, undo, bookmarks (generation g)
//   timeline/segments/N.seg  append-only input records (RLE + CRC32)
//   states/N.state           checkpoint states (CRC32)
//   states/practice-<slot>-<seq>.state  A/B practice slot A states (CRC32)
//   metadata/bookmarks.json  human-readable mirror of the bookmarks in index.json
//   metadata/practice.json   human-readable mirror of the practice slots in index.json
//   journal/journal.bin      append-only records since generation g (CRC32 per record)
// Full save = write new data files, index (g+1), mirror, manifest (g+1), reset journal, GC.
// Autosave = append journal records + fsync. Every write is atomic (temp+fsync+rename) or an
// fsync'ed append that is truncated back on failure.
#pragma once
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "session/Session.h"

namespace rn {

// 2: adds practice slots (meta.practice, practice state files, journal PRACTICE_STATE).
// Version-1 projects open unchanged (no slots); every save writes version 2.
constexpr uint32_t kProjectFormatVersion = 2;
constexpr uint32_t kInputFormatVersion = 1;

struct OpenReport {
  bool journalApplied = false;   // crash recovery replayed journal records
  bool journalTornTail = false;  // an incomplete trailing journal record was discarded
  std::vector<std::string> droppedStates;  // only with SessionOptions::dropCorruptStates
  std::vector<std::string> droppedPracticeSlots;  // only with SessionOptions::dropCorruptPractice
};

class ProjectStore {
 public:
  static Status create(const std::string& dir, Session& s, std::unique_ptr<ProjectStore>& out);
  static Status open(const std::string& dir, const std::string& romOverride, const SessionOptions& opt,
                     std::unique_ptr<Session>& out, OpenReport* report = nullptr);
  // Reads only manifest.json (for UI "open recent"/diagnostics).
  static Status readManifest(const std::string& dir, std::string& json);

  Status fullSave(Session& s);
  Status autosave(Session& s);
  const std::string& dir() const { return dir_; }
  uint64_t generation() const { return gen_; }

 private:
  explicit ProjectStore(std::string dir) : dir_(std::move(dir)) {}
  std::string path(const std::string& rel) const;
  Status resetJournal();
  std::string metaJson(const Session& s) const;
  Status applyMeta(Session& s, const std::string& json);

  struct PracticeBlob {
    Status status;
    std::vector<uint8_t> data;
    std::string compat;
    uint32_t stateFormatVersion = 0;
    bool fromFile = false;
  };
  // Open only: A states loaded from files/journal, keyed by (slot, stateSeq); per-slot errors.
  std::map<std::pair<int, uint64_t>, PracticeBlob> practicePool_;
  std::array<Status, kPracticeSlots> slotErr_;

  std::string dir_;
  uint64_t gen_ = 0;
  uint64_t journalBase_ = UINT64_MAX;
  std::string lastMeta_;
};

}  // namespace rn
