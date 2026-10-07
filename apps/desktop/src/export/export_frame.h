// Frame helpers of the MP4 exporters (export/mp4_export.h; Linux: FFmpeg, Windows: Media
// Foundation), shared so both write the same pixels: nearest-neighbour scaling into the canvas
// (MP4Exporter.scale on macOS) and the exact BGRA -> BT.709 limited-range 4:2:0 conversion.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "replaynes/frontend.h"

namespace rnl::exportframe {

/// canvas (BGRA, w x h, both even) -> Y plane + either separate U/V planes (YUV420P) or one
/// interleaved UV plane (NV12, v == nullptr). Exact integer BT.709 limited range; chroma is the
/// mean of each 2x2 block (centred).
void convertToYuv(const uint8_t* canvas, size_t stride, int w, int h, uint8_t* y, int yStride, uint8_t* u, int uStride,
                  uint8_t* v, int vStride);

/// BGRA 256x240 -> canvas with black borders (cols / rows: rnf_export_column_map / row_map).
void scaleNearest(const uint32_t* src, uint8_t* canvas, size_t stride, const rnf_export_geometry& g, const std::vector<int>& cols,
                  const std::vector<int>& rows);

/// Start of video frame `n` (frames since the export start) in 100 ns units, rounded to nearest:
/// n * 655171 / 39375000 s (Media Foundation's time base; durations are differences of these, so
/// rounding never accumulates).
int64_t frameTime100ns(uint64_t n);
/// Start of audio sample `n` (at 48 kHz) in 100 ns units (exact: 10^7 / 48000 = 625 / 3).
int64_t sampleTime100ns(uint64_t n);

}  // namespace rnl::exportframe
