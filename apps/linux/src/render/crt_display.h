// The CRT on the live display path (VkRenderer): the Vulkan CrtRenderer driven by the shared
// CrtDisplayPolicy (render/crt_display_policy.h: stored picture + input choice, tube size,
// build-ahead and adaptive resolution from the GPU times; the Direct3D 11 renderer uses the same).
// Records the build passes and the show pass.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>

#include "render/crt_display_policy.h"
#include "render/crt_renderer.h"
#include "render/post_process.h"

namespace rnl {

class CrtDisplay {
 public:
  CrtDisplay() = default;
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
  bool hasFrame() const { return policy_.hasFrame(); }

  /// Applies settings / destination (tube size = dst x scale, capped) without recording anything.
  void configure(const DisplayPostProcess& pp, const CrtRect& dst, double cropFraction);
  /// Records the build passes of the stored picture. False when nothing was recorded (no plan yet).
  bool build(VkCommandBuffer cmd);
  bool canShow() const { return renderer_ && renderer_->hasOutput(); }
  /// A tube plan is being built / waits to be adopted by the next build.
  bool planPending() { return renderer_ && renderer_->planPending(); }
  void show(VkCommandBuffer cmd, int targetWidth, int targetHeight, const CrtRect& dst, double cropFraction);

  /// Once per frame: GPU times of finished builds -> build-ahead / adaptive resolution.
  /// `budget` = seconds the GPU has between the commit and the target vblank at the maximum lead.
  void update(double budget, bool allowBuildAhead, bool adaptive);
  bool pipelined() const { return policy_.pipelined(); }
  double gpuP90() const { return policy_.gpuP90(); }
  /// Status for the UI (tube size, GPU ms, scale, build-ahead, RF/RGB).
  void fillStatus(PostProcessStatus* s) const;

 private:
  std::unique_ptr<CrtRenderer> renderer_;
  CrtDisplayPolicy policy_;
};

}  // namespace rnl
