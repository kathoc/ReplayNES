// The CRT on the live display path (VkRenderer): keeps the last picture + signal, sizes the tube for
// the destination rectangle, records the build passes and the show pass, measures their GPU time
// and decides (from it):
//  * build-ahead (rnf_build_ahead of the shared core, like MetalView on macOS): when the build's
//    p90 GPU time no longer fits in what the input lead leaves before the target vblank, the
//    picture of frame N is built after frame N's present and shown by the next present (every
//    picture one frame later, instead of missing its vblank);
//  * adaptive resolution: when the p90 GPU time exceeds 80 % of the NES frame period (the GPU
//    can't sustain 60 builds/s) the tube is rendered smaller (steps of 0.85, at least 512 wide)
//    and enlarged by the show pass (bilinear in linear light); it grows back below 55 %.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <vector>

#include "render/crt_renderer.h"
#include "render/post_process.h"

struct rnf_build_ahead;

namespace rnl {

class CrtDisplay {
 public:
  CrtDisplay();
  ~CrtDisplay();
  CrtDisplay(const CrtDisplay&) = delete;
  CrtDisplay& operator=(const CrtDisplay&) = delete;

  /// Creates the pipeline on first use. False (error set) when the device can't run it.
  bool ensure(const CrtVulkanContext& ctx, VkRenderPass pass, std::string* error);
  /// Frees the GPU resources (waits for the device). The next ensure() starts over.
  void release();
  bool active() const { return renderer_ != nullptr; }

  /// A new displayed picture (flash-filtered) and its signal.
  void store(const uint32_t* pixels, const FrameSignal* signal);
  bool hasFrame() const { return valid_; }

  /// Applies settings / destination (tube size = dst x scale, capped) without recording anything.
  void configure(const DisplayPostProcess& pp, const CrtRect& dst, double cropFraction);
  /// Records the build passes of the stored picture. False when nothing was recorded (no plan yet).
  bool build(VkCommandBuffer cmd);
  bool canShow() const { return renderer_ && renderer_->hasOutput(); }
  void show(VkCommandBuffer cmd, int targetWidth, int targetHeight, const CrtRect& dst, double cropFraction);

  /// Once per frame: GPU times of finished builds -> build-ahead / adaptive resolution.
  /// `budget` = seconds the GPU has between the commit and the target vblank at the maximum lead.
  void update(double budget, bool allowBuildAhead, bool adaptive);
  bool pipelined() const { return pipelined_; }
  double gpuP90() const { return p90_; }
  /// Status for the UI (tube size, GPU ms, scale, build-ahead, RF/RGB).
  void fillStatus(PostProcessStatus* s) const;

 private:
  std::unique_ptr<CrtRenderer> renderer_;
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
