// Exact NTSC frame timing for an MP4 written by a muxer that rounds the video track's timescale
// (Media Foundation's MPEG-4 sink picks frames-per-second x 1000 = 60098, so every frame lasts
// 999 or 1000 ticks): rewrites the moov box so that the video track has the timescale 39375000
// and exactly 655171 ticks per frame (video frame f starts at f * 655171 / 39375000 s, as the
// FFmpeg exporter writes it), with 64-bit durations where needed. The samples, their order and
// the audio track are untouched; composition offsets / edit lists are rescaled (all-zero offsets
// are dropped). Needs moov after mdat (the end of the file): it is rewritten in place.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace rnl {

/// `frames` = video samples expected in the file (one per frame). False + *error when the file is
/// not as expected (then it is left as it was).
bool retimeMp4Video(const std::string& path, uint64_t frames, uint32_t timescale, uint32_t frameTicks, std::string* error);

}  // namespace rnl
