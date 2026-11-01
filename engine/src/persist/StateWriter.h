// The state files of new sparse checkpoints, written on a thread of their own (ProjectStore::autosave):
// the checksums, the file and its fsync. On the frame thread a large state (Sega System 32: 22 MB)
// stopped the picture for tens of milliseconds at every autosave that had one to write.
//
// A job reads the checkpoint's bytes in place: the frame thread does not change or free them while
// the job runs (Checkpoint::writing; a checkpoint removed meanwhile keeps its bytes in
// CheckpointStore::held_ until no job runs).
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/Status.h"

namespace rn {

class StateWriter {
 public:
  struct Job {
    uint64_t id = 0;
    std::string path;
    std::vector<uint8_t> header;  // the state file before the bytes (ProjectStore: encodeState's layout)
    const uint8_t* data = nullptr;
    size_t n = 0;
    uint32_t crc = 0;  // done: of the bytes (Checkpoint::crc)
    Status status;
  };
  StateWriter() = default;
  StateWriter(const StateWriter&) = delete;
  StateWriter& operator=(const StateWriter&) = delete;
  ~StateWriter();  // waits for the jobs

  void submit(Job j);
  /// The jobs finished since the last call (in submission order).
  std::vector<Job> takeDone();
  /// Waits until every submitted job is finished.
  void drain();
  /// Submitted and not yet taken back by takeDone.
  size_t outstanding() const;

 private:
  void run();
  mutable std::mutex m_;
  std::condition_variable cv_, idle_;
  std::deque<Job> queue_;
  std::vector<Job> done_;
  bool busy_ = false, quit_ = false;
  std::thread thread_;
};

}  // namespace rn
