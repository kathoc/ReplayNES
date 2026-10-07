// The live-view logic of the CRT display shared by the Vulkan (apps/linux/src/render/crt_display.h)
// and Direct3D 11 (apps/windows/src/crt_d3d11.h) renderers - no graphics API here:
//  * keeps the last picture + its signal and picks the input (RF from the PPU codes, or the
//    flash-filtered RGB picture when the filter altered the frame / the core has no codes);
//  * sizes the tube for the destination (1:1 at scale 1, 4:3, the nesterm cap);
//  * from the GPU times of the builds decides
//    - build-ahead (rnf_build_ahead of the shared core, like MetalView on macOS): when the build's
//      p90 GPU time no longer fits in what the input lead leaves before the target vblank, the
//      picture of frame N is built after frame N's present and shown by the next present;
//    - adaptive resolution: when the p90 GPU time exceeds 80 % of the NES frame period the tube
//      is rendered smaller (steps of 0.85, at least 512 wide) and enlarged by the show pass
//      (bilinear in linear light); it grows back below 55 %.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/crt_types.h"
#include "render/post_process.h"

struct rnf_build_ahead;

namespace rnl {

class CrtDisplayPolicy {
 public:
  CrtDisplayPolicy();
  ~CrtDisplayPolicy();
  CrtDisplayPolicy(const CrtDisplayPolicy&) = delete;
  CrtDisplayPolicy& operator=(const CrtDisplayPolicy&) = delete;

  /// A new pipeline: forget the GPU times and the build-ahead state.
  void reset();
  /// A new displayed picture (flash-filtered) and its signal.
  void store(const uint32_t* pixels, const FrameSignal* signal);
  bool hasFrame() const { return valid_; }
  /// The stored picture as CRT input (pointers into this object) and its machine frame ordinal.
  CrtInput input() const;
  uint64_t ordinal() const { return signal_.ordinal; }
  /// Tube size for a destination rectangle at the current adaptive scale.
  void tubeSize(const DisplayPostProcess& pp, const CrtRect& dst, double cropFraction, int* w, int* h) const;
  /// Once per frame: GPU times of finished builds -> build-ahead / adaptive resolution.
  /// `budget` = seconds the GPU has between the commit and the target vblank at the maximum lead;
  /// `tubeWidth` = the tube width in use.
  void update(const std::vector<double>& gpuTimes, double budget, bool allowBuildAhead, bool adaptive, int tubeWidth);
  bool pipelined() const { return pipelined_; }
  double gpuP90() const { return p90_; }
  /// Status for the UI (GPU ms, scale, build-ahead); the caller adds tube size and RF/RGB.
  void fillStatus(PostProcessStatus* s) const;

 private:
  rnf_build_ahead* ahead_ = nullptr;
  std::vector<uint32_t> pixels_;
  std::vector<uint16_t> codes_;
  FrameSignal signal_;
  bool hasCodes_ = false, valid_ = false;
  std::vector<double> times_;  // recent GPU build times (window)
  size_t sinceChange_ = 0;
  double scale_ = 1, p50_ = 0, p90_ = 0;
  bool pipelined_ = false;
};

}  // namespace rnl
