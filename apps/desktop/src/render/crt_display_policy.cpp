// SPDX-License-Identifier: GPL-2.0-or-later
#include "render/crt_display_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

void crtTubeSize(double dstW, double dstH, double cropFraction, int maxWidth, int* w, int* h) {
  double fullH = dstH / std::max(0.5, 1 - 2 * cropFraction);
  double ww = std::min(dstW, fullH * 4 / 3);
  ww = std::max(256.0, std::min(double(maxWidth), std::round(ww / 4) * 4));
  *w = int(ww);
  *h = int(ww) * 3 / 4;
}

CrtDisplayPolicy::CrtDisplayPolicy() : ahead_(rnf_build_ahead_new()) {
  pixels_.assign(size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT, 0xFF000000u);
  codes_.assign(size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT, 0x0F);
}

CrtDisplayPolicy::~CrtDisplayPolicy() { rnf_build_ahead_free(ahead_); }

void CrtDisplayPolicy::reset() {
  times_.clear();
  sinceChange_ = 0;
  pipelined_ = false;
  rnf_build_ahead_reset(ahead_);
}

void CrtDisplayPolicy::store(const uint32_t* pixels, const FrameSignal* signal) {
  std::memcpy(pixels_.data(), pixels, pixels_.size() * 4);
  signal_ = signal ? *signal : FrameSignal();
  hasCodes_ = signal && signal->codes;
  if (hasCodes_) std::memcpy(codes_.data(), signal->codes, codes_.size() * 2);
  signal_.codes = nullptr;
  valid_ = true;
}

CrtInput CrtDisplayPolicy::input() const {
  CrtInput in;
  if (hasCodes_ && !signal_.flashAltered) {
    in.kind = CrtInputKind::codes;
    in.codes = codes_.data();
    in.burstPhase = signal_.burstPhase;
  } else {
    in.kind = CrtInputKind::rgb;
    in.rgb = pixels_.data();
  }
  return in;
}

void CrtDisplayPolicy::tubeSize(const DisplayPostProcess& pp, const CrtRect& dst, double cropFraction, int* w, int* h) const {
  // The tube at scale_ of the destination (1:1 at scale 1), 4:3, nesterm's cap.
  crtTubeSize(std::max(1.0, dst.w * scale_), std::max(1.0, dst.h * scale_), cropFraction, std::max(256, pp.maxTubeWidth), w, h);
}

void CrtDisplayPolicy::update(const std::vector<double>& gpuTimes, double budget, bool allowBuildAhead, bool adaptive,
                              int tubeWidth) {
  for (double t : gpuTimes) {
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
  static const bool forceAhead = std::getenv("REPLAYNES_CRT_BUILD_AHEAD") != nullptr;  // verification
  if (forceAhead) {
    pipelined_ = true;
  } else if (allowBuildAhead && budget > 0) {
    rnf_build_ahead_update(ahead_, budget);
    pipelined_ = rnf_build_ahead_active(ahead_) != 0;
  } else {
    pipelined_ = false;
  }
  if (adaptive && sinceChange_ >= kWindow && times_.size() >= kWindow) {
    if (p90_ > kDownAt * kFramePeriod && tubeWidth * kScaleStep >= kMinWidth) {
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

void CrtDisplayPolicy::fillStatus(PostProcessStatus* s) const {
  s->scale = scale_;
  s->gpuMsP50 = p50_ * 1000;
  s->gpuMsP90 = p90_ * 1000;
  s->buildAhead = pipelined_;
}

}  // namespace rnl
