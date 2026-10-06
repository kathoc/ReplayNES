// SPDX-License-Identifier: GPL-2.0-or-later
#include "render/crt_display.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "replaynes/frontend.h"
#include "replaynes/replaynes.h"

namespace rnl {

namespace {
constexpr size_t kWindow = 60;           // GPU times per decision
constexpr double kScaleStep = 0.85, kMinWidth = 512;
constexpr double kDownAt = 0.80, kUpAt = 0.55;  // of the NES frame period
const double kFramePeriod = double(RN_FPS_DEN) / double(RN_FPS_NUM);
}  // namespace

CrtDisplay::CrtDisplay() : ahead_(rnf_build_ahead_new()) {
  pixels_.assign(size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT, 0xFF000000u);
  codes_.assign(size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT, 0x0F);
}

CrtDisplay::~CrtDisplay() {
  release();
  rnf_build_ahead_free(ahead_);
}

bool CrtDisplay::ensure(const CrtVulkanContext& ctx, VkRenderPass pass, std::string* error) {
  if (renderer_) return true;
  auto r = std::make_unique<CrtRenderer>();
  if (!r->init(ctx, pass, error)) return false;
  renderer_ = std::move(r);
  times_.clear();
  sinceChange_ = 0;
  pipelined_ = false;
  rnf_build_ahead_reset(ahead_);
  return true;
}

void CrtDisplay::release() {
  if (!renderer_) return;
  renderer_->shutdown();
  renderer_.reset();
  pipelined_ = false;
  rnf_build_ahead_reset(ahead_);
}

void CrtDisplay::store(const uint32_t* pixels, const FrameSignal* signal) {
  std::memcpy(pixels_.data(), pixels, pixels_.size() * 4);
  signal_ = signal ? *signal : FrameSignal();
  hasCodes_ = signal && signal->codes;
  if (hasCodes_) std::memcpy(codes_.data(), signal->codes, codes_.size() * 2);
  signal_.codes = nullptr;
  valid_ = true;
}

void CrtDisplay::configure(const DisplayPostProcess& pp, const CrtRect& dst, double cropFraction) {
  if (!renderer_) return;
  int tw = 0, th = 0;
  // The tube at scale_ of the destination (1:1 at scale 1), 4:3, nesterm's cap.
  double s = scale_;
  CrtRenderer::tubeSize(std::max(1.0, dst.w * s), std::max(1.0, dst.h * s), cropFraction, std::max(256, pp.maxTubeWidth), &tw, &th);
  renderer_->configure(pp.crtSettings, tw, th);
}

bool CrtDisplay::build(VkCommandBuffer cmd) {
  if (!renderer_ || !valid_) return false;
  // RF path from the raw PPU codes; the (flash-filtered) RGB picture (nesterm's synthetic-RGB
  // source) when the photosensitive filter altered the frame or the core has no codes.
  CrtRenderer::Input in;
  if (hasCodes_ && !signal_.flashAltered) {
    in.kind = CrtRenderer::InputKind::codes;
    in.codes = codes_.data();
    in.burstPhase = signal_.burstPhase;
  } else {
    in.kind = CrtRenderer::InputKind::rgb;
    in.rgb = pixels_.data();
  }
  return renderer_->encode(cmd, in, signal_.ordinal);
}

void CrtDisplay::show(VkCommandBuffer cmd, int targetWidth, int targetHeight, const CrtRect& dst, double cropFraction) {
  if (renderer_) renderer_->encodeShow(cmd, targetWidth, targetHeight, dst, cropFraction);
}

void CrtDisplay::update(double budget, bool allowBuildAhead, bool adaptive) {
  if (!renderer_) return;
  for (double t : renderer_->takeGpuTimes()) {
    rnf_build_ahead_add(ahead_, t);
    times_.push_back(t);
    if (times_.size() > kWindow) times_.erase(times_.begin());
    sinceChange_ += 1;
  }
  if (!times_.empty()) {
    std::vector<double> s = times_;
    std::sort(s.begin(), s.end());
    p50_ = s[s.size() / 2];
    p90_ = s[size_t(std::round(double(s.size() - 1) * 0.9))];
  }
  if (allowBuildAhead && budget > 0) {
    rnf_build_ahead_update(ahead_, budget);
    pipelined_ = rnf_build_ahead_active(ahead_) != 0;
  } else {
    pipelined_ = false;
  }
  if (adaptive && sinceChange_ >= kWindow && times_.size() >= kWindow) {
    int ow = renderer_->outputWidth();
    if (p90_ > kDownAt * kFramePeriod && ow * kScaleStep >= kMinWidth) {
      scale_ *= kScaleStep;
      sinceChange_ = 0;
      times_.clear();
    } else if (p90_ < kUpAt * kFramePeriod && scale_ < 1) {
      scale_ = std::min(1.0, scale_ / kScaleStep);
      sinceChange_ = 0;
      times_.clear();
    }
  } else if (!adaptive) {
    scale_ = 1;
  }
}

void CrtDisplay::fillStatus(PostProcessStatus* s) const {
  s->tubeWidth = renderer_ ? renderer_->outputWidth() : 0;
  s->tubeHeight = renderer_ ? renderer_->outputHeight() : 0;
  s->scale = scale_;
  s->gpuMsP50 = p50_ * 1000;
  s->gpuMsP90 = p90_ * 1000;
  s->buildAhead = pipelined_;
  s->usedCodes = renderer_ && renderer_->usedCodes();
}

}  // namespace rnl
