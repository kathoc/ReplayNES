// GPU/COM-independent input preparation for the Media Foundation exporter.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>
#include "replaynes/frontend.h"

namespace rnl::mfmedia {

// Round an absolute position, never individual durations. -1 means it cannot be represented.
inline int64_t time100ns(uint64_t n, uint64_t ticks, uint32_t timescale) {
  if (!timescale || (ticks && n > UINT64_MAX / ticks)) return -1;
  const uint64_t total = n * ticks, seconds = total / timescale;
  const uint64_t fraction = ((total % timescale) * 10000000 + timescale / 2) / timescale;
  if (seconds > (uint64_t(INT64_MAX) - fraction) / 10000000) return -1;
  return int64_t(seconds * 10000000 + fraction);
}

// Refit a changed source into the original movie canvas, including its rotation and PAR.
inline bool resizeGeometry(const rnf_export_settings& settings, const rn_media_info& media,
                           uint32_t width, uint32_t height, int canvasWidth, int canvasHeight,
                           rnf_export_geometry* geometry) {
  if (rnf_export_validate_media(&settings, &media, width, height, nullptr) != RN_OK) return false;
  auto fixed = settings;
  fixed.preset.kind = RNF_PRESET_CANVAS;
  fixed.preset.width = canvasWidth;
  fixed.preset.height = canvasHeight;
  rnf_export_geometry_compute_media(&fixed, &media, width, height, geometry);
  return geometry->canvas_width > 0 && geometry->canvas_height > 0;
}

// Microsoft AAC accepts 44.1 and 48 kHz. Keep either unchanged, choose the nearest otherwise.
inline uint32_t aacRate(double rate) {
  return std::abs(rate - 44100) < std::abs(rate - 48000) ? 44100 : 48000;
}

// Counts are sample frames; interleaved channels are never counted as elapsed time.
// Staging ratios within [0.5, 2] avoids the frontend resampler's ratio clamp, including
// low/high input rates. No resampler at unity: NES PCM is passed through byte-for-byte.
class Audio {
 public:
  bool init(double inputRate, uint32_t channels) {
    if (!std::isfinite(inputRate) || inputRate <= 0 || (channels != 1 && channels != 2)) return false;
    stages_.clear();
    channels_ = channels;
    rate_ = aacRate(inputRate);
    ratio_ = double(rate_) / inputRate;
    input_ = output_ = 0;
    pending_.clear();
    if (!std::isfinite(ratio_) || ratio_ <= 0) return false;
    double remaining = ratio_;
    while (remaining != 1.0) {
      const double step = std::max(0.5, std::min(2.0, remaining));
      Stage s;
      s.ratio = step;
      s.state.reset(rnf_resampler_new_with_channels(channels));
      if (!s.state) return false;
      stages_.push_back(std::move(s));
      remaining /= step;
    }
    return true;
  }
  uint32_t rate() const { return rate_; }
  uint64_t frames() const { return output_; }
  bool process(const int16_t* input, size_t frames, std::vector<int16_t>& result) {
    input_ += frames;
    for (auto& stage : stages_) {
      const size_t capacity = rnf_resampler_output_bound(frames);
      stage.buffer.resize(capacity * channels_);
      size_t count = 0;
      if (rnf_resampler_process(stage.state.get(), input, frames, stage.ratio,
                                stage.buffer.data(), capacity, &count) != RN_OK) return false;
      input = stage.buffer.data();
      frames = count;
    }
    // A stage can be ahead by a sub-sample. Keep the streaming output within the final
    // soundtrack length; finish() supplies the delayed tail without lengthening the audio.
    pending_.insert(pending_.end(), input, input + frames * channels_);
    const uint64_t target = uint64_t(std::llround(double(input_) * ratio_));
    frames = size_t(std::min<uint64_t>(pending_.size() / channels_, target > output_ ? target - output_ : 0));
    result.assign(pending_.begin(), pending_.begin() + frames * channels_);
    pending_.erase(pending_.begin(), pending_.begin() + frames * channels_);
    output_ += frames;
    return true;
  }
  void finish(std::vector<int16_t>& result) {
    const uint64_t target = uint64_t(std::llround(double(input_) * ratio_));
    pending_.resize(size_t(target - output_) * channels_, 0);
    result.swap(pending_);
    pending_.clear();
    output_ = target;
  }
 private:
  struct Stage {
    double ratio = 1;
    std::unique_ptr<rnf_resampler, void (*)(rnf_resampler*)> state{nullptr, rnf_resampler_free};
    std::vector<int16_t> buffer;
  };
  std::vector<Stage> stages_;
  std::vector<int16_t> pending_;
  double ratio_ = 1;
  uint32_t channels_ = 1, rate_ = 48000;
  uint64_t input_ = 0, output_ = 0;
};
}  // namespace rnl::mfmedia
