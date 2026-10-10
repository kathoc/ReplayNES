// Structural self-check of a finished (non-fragmented) MP4 file, run by the exporters before the
// file is handed to the user: the top-level boxes span the whole file (no truncated / unfinished
// box), there is exactly one moov, every chunk of every track (stco / co64 + stsc + stsz) lies
// inside an mdat payload, and for H.264 (avc1) the first sample of every chunk and the last sample
// parse as length-prefixed NAL units that add up to the sample size - i.e. the chunk offsets
// really point at the samples, also beyond 4 GiB (a 32-bit chunk offset that wrapped fails here).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rnl {

struct Mp4TrackSummary {
  std::string handler;     // "vide", "soun", ...
  std::string codec;       // sample entry: "avc1", "mp4a", ...
  uint32_t timescale = 0;  // mdhd
  uint64_t duration = 0;   // mdhd (track timescale)
  uint64_t samples = 0;    // stsz
  uint64_t chunks = 0;
  bool co64 = false;       // 64-bit chunk offsets
  uint64_t dataEnd = 0;    // end of the last sample (file offset)
};

struct Mp4Summary {
  uint64_t fileSize = 0;
  uint64_t mdatBytes = 0;  // payload bytes of all mdat boxes
  bool moovAtEnd = false;
  std::vector<Mp4TrackSummary> tracks;
};

/// False + *error (one line, "MP4 check: ...") when the file is not a complete, consistent MP4.
bool checkMp4File(const std::string& path, Mp4Summary* summary, std::string* error);

}  // namespace rnl
