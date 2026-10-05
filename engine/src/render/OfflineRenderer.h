// OfflineRenderer: re-executes a SNAPSHOT of the active take on a fresh core instance from
// power-on at logical time, yielding video + PCM per frame (for MP4 export). It shares nothing
// mutable with the Session, so it may run on another thread while the session keeps playing.
// Output frame i has video PTS i*655171/39375000 s; its audio starts at sample audioSamplesBefore(i).
#pragma once
#include <memory>
#include <vector>

#include "session/Session.h"
#include "util/Hash.h"

namespace rn {

class OfflineRenderer {
 public:
  // end == 0 means the take length. Frames before `start` are emulated (pre-roll) but not output.
  static Status create(const Session& s, uint64_t start, uint64_t end, std::unique_ptr<OfflineRenderer>& out);
  uint64_t totalFrames() const { return end_ - start_; }
  uint64_t framesDone() const { return pos_ > start_ ? pos_ - start_ : 0; }
  // Returns Err::EndOfTake when all frames were produced. Buffers valid until the next call.
  Status next(const uint32_t** video, const int16_t** audio, size_t* samples, uint64_t* frameIndex);
  uint64_t hash() const { return running_.digest(); }  // over all (videoHash, audioHash) pairs output so far

 private:
  OfflineRenderer() = default;
  std::vector<InputRecord> recs_;
  std::vector<uint8_t> rom_;
  std::unique_ptr<ICore> core_;
  uint64_t start_ = 0, end_ = 0, pos_ = 0;
  bool started_ = false;
  Hasher64 running_;
};

}  // namespace rn
