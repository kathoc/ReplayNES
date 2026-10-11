// SPDX-License-Identifier: GPL-2.0-or-later
#include "render/crt_display.h"

#include <cstdio>

namespace rnl {

CrtDisplay::~CrtDisplay() { release(); }

bool CrtDisplay::ensure(const CrtVulkanContext& ctx, VkRenderPass pass, std::string* error) {
  if (renderer_) return true;
  auto r = std::make_unique<CrtRenderer>();
  if (!r->init(ctx, pass, error)) return false;
  renderer_ = std::move(r);
  policy_.reset();
  return true;
}

void CrtDisplay::release() {
  if (!renderer_) return;
  renderer_->shutdown();
  renderer_.reset();
  policy_.reset();
}

void CrtDisplay::store(const uint32_t* pixels, const FrameSignal* signal) { policy_.store(pixels, signal); }

void CrtDisplay::configure(const DisplayPostProcess& pp, const CrtRect& dst, double cropFraction) {
  if (!renderer_) return;
  int tw = 0, th = 0;
  policy_.tubeSize(pp, dst, cropFraction, &tw, &th);
  renderer_->configure(pp.crtSettings, tw, th);
}

bool CrtDisplay::build(VkCommandBuffer cmd) {
  if (!renderer_ || !policy_.hasFrame()) return false;
  return renderer_->encode(cmd, policy_.input(), policy_.ordinal());
}

void CrtDisplay::show(VkCommandBuffer cmd, int targetWidth, int targetHeight, const CrtRect& dst, double cropFraction) {
  if (renderer_) renderer_->encodeShow(cmd, targetWidth, targetHeight, dst, cropFraction);
}

void CrtDisplay::update(double budget, bool allowBuildAhead, bool adaptive) {
  if (!renderer_) return;
  policy_.update(renderer_->takeGpuTimes(), budget, allowBuildAhead, adaptive, renderer_->outputWidth());
}

std::string CrtDisplay::state() const {
  if (!renderer_) return "CRT off";
  char b[160];
  std::snprintf(b, sizeof b, "CRT tube %dx%d %d lines, output %s%s, state resets %d", renderer_->outputWidth(), renderer_->outputHeight(),
                renderer_->renderedLines(), renderer_->hasOutput() ? "yes" : "no", renderer_->planPending() ? ", plan pending" : "",
                renderer_->stateResets());
  return b;
}

void CrtDisplay::fillStatus(PostProcessStatus* s) const {
  policy_.fillStatus(s);
  s->tubeWidth = renderer_ ? renderer_->outputWidth() : 0;
  s->tubeHeight = renderer_ ? renderer_->outputHeight() : 0;
  s->usedCodes = renderer_ && renderer_->usedCodes();
}

}  // namespace rnl
