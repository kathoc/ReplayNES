// INTERIM FRONTEND LOGIC (Steam Deck plan, Step 2 skeleton).
// C++ port of the pure pacing logic in apps/macos/Sources/Core/DisplayPacing.swift
// (InputDeadline, AudioRateControl, the refresh estimate of DisplayCadence) plus the cadence
// classification the Linux display loop needs (integer lock, 3:2 lock for 90 Hz, free).
// The shared C++ frontend core (frontend/, plan Step 1/3) takes this over: when it lands, replace
// this header with the core's types and keep the Linux-only parts (present_clock.h) as they are.
// Pure: no clocks, no I/O. Times in seconds.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "replaynes/frontend.h"
#include "replaynes/replaynes.h"

namespace rnl::interim {

/// Exact NTSC frame period (RN_FPS_DEN / RN_FPS_NUM s, 60.0988 Hz).
constexpr double kNesFramePeriod = double(RN_FPS_DEN) / double(RN_FPS_NUM);

/// How display refreshes map to emulated frames (DisplayCadence on macOS).
///  * Locked k: refresh x k == NES period within 0.5 % (60 Hz: k = 1, 120 Hz: k = 2). Every k-th
///    refresh starts a frame; emulation runs at refresh / k (60.000 Hz instead of 60.0988 Hz).
///  * Locked 3:2: refresh x 1.5 == NES period within 0.5 % (90 Hz panels, e.g. Steam Deck OLED):
///    frames alternate 2 and 1 refreshes on screen (a regular pattern at exactly refresh / 1.5).
///  * Free: other rates keep the NTSC rate and take the refresh nearest to the ideal frame time.
///  * Slower: displays slower than the NTSC rate (40-59 Hz, nested compositors, 30 Hz): every
///    refresh shows a frame and up to 2 frames are emulated for it (shared core
///    rnf_frame_budget), so emulation keeps the NTSC rate.
struct Cadence {
  static constexpr double kLockTolerance = 0.005;
  enum class Kind { unknown, locked, three_two, free, slower };
  Kind kind = Kind::unknown;
  int k = 0;  // refreshes per frame when locked

  static Cadence classify(double refresh, double framePeriod = kNesFramePeriod) {
    Cadence c;
    if (!(refresh > 0)) return c;
    double kk = std::round(framePeriod / refresh);
    if (kk >= 1 && std::fabs(kk * refresh - framePeriod) / framePeriod <= kLockTolerance) {
      c.kind = Kind::locked;
      c.k = int(kk);
    } else if (std::fabs(1.5 * refresh - framePeriod) / framePeriod <= kLockTolerance) {
      c.kind = Kind::three_two;
    } else if (rnf_display_slower_than_frames(refresh, framePeriod)) {
      c.kind = Kind::slower;
    } else {
      c.kind = Kind::free;
    }
    return c;
  }

  /// Frames per second emulated with this cadence (the NES rate when free / unknown).
  double emulationRate(double refresh) const {
    switch (kind) {
      case Kind::locked: return 1.0 / (k * refresh);
      case Kind::three_two: return 1.0 / (1.5 * refresh);
      default: return 1.0 / kNesFramePeriod;
    }
  }
};

/// Refresh interval estimate from display timestamps: the median of the last 15 plausible
/// deltas, refined by an EMA (DisplayCadence.refresh on macOS). Deltas spanning several refreshes
/// (k > 1, a missed refresh) are divided by their refresh count against the current estimate.
struct RefreshEstimator {
  static constexpr double kMinRefresh = 1.0 / 500, kMaxRefresh = 1.0 / 24;
  static constexpr int kWindow = 15;
  double refresh = 0;  // 0 = unknown

  void seed(double r) {
    if (r >= kMinRefresh && r <= kMaxRefresh && refresh == 0) refresh = r;
  }
  /// Two display timestamps `d` seconds apart.
  void observeDelta(double d) {
    if (!(d > 0)) return;
    double one = d;
    if (refresh > 0) {
      double n = std::round(d / refresh);
      if (n < 1 || n > 8 || std::fabs(d - n * refresh) > 0.25 * refresh) return;
      one = d / n;
    }
    if (one < kMinRefresh || one > kMaxRefresh) return;
    if ((int)deltas_.size() < kWindow) deltas_.push_back(one);
    else { deltas_[head_] = one; head_ = (head_ + 1) % kWindow; }
    std::vector<double> s = deltas_;
    std::sort(s.begin(), s.end());
    double median = s[s.size() / 2];
    if (refresh == 0 || std::fabs(refresh - median) > 0.1 * median) refresh = median;
    else refresh += (one - refresh) * 0.05;
  }

 private:
  std::vector<double> deltas_;
  int head_ = 0;
};

/// Just-in-time input sampling (InputDeadline on macOS): input is sampled `lead` seconds before
/// the frame's deadline. lead = p99.5 of the recent sample -> submit work + margin + penalty; the
/// penalty grows on every frame that missed its refresh and decays while none do.
/// On Linux the deadline is the predicted vblank itself (the compositor's latch point before it
/// is unknown), so the penalty also learns the compositor's latch offset: its cap is larger than on
/// macOS (set from the refresh by the caller).
struct InputDeadline {
  double margin = 0.0015;
  double minLead = 0.002;
  double maxLead = 0.010;
  double missPenalty = 0.0005;
  double maxPenalty = 0.003;
  int decayAfter = 600;          // clean frames before the penalty shrinks (~10 s)
  double penaltyDecay = 0.0001;  // 0.1 ms
  static constexpr int kWindow = 600;
  static constexpr double kQuantile = 0.995;
  static constexpr double kBinWidth = 0.0001;
  static constexpr int kBinCount = 101;

  double penalty = 0;
  uint64_t misses = 0;

  double workQuantile() const {
    if (filled_ == 0) return 0;
    int need = int(std::ceil(filled_ * kQuantile));
    int sum = 0;
    for (int i = 0; i < kBinCount; ++i) {
      sum += bins_[i];
      if (sum >= need) return (i + 1) * kBinWidth;
    }
    return kBinCount * kBinWidth;
  }
  double lead() const { return std::min(maxLead, std::max(minLead, workQuantile() + margin + penalty)); }

  void observeWork(double seconds) {
    int bin = std::min(kBinCount - 1, std::max(0, int(seconds / kBinWidth)));
    if (filled_ == kWindow) bins_[ring_[head_]] -= 1;
    else filled_ += 1;
    ring_[head_] = uint8_t(bin);
    bins_[bin] += 1;
    head_ = (head_ + 1) % kWindow;
    if (++clean_ >= decayAfter) {
      clean_ = 0;
      penalty = std::max(0.0, penalty - penaltyDecay);
    }
  }
  void observeMiss() {
    misses += 1;
    clean_ = 0;
    penalty = std::min(maxPenalty, penalty + missPenalty);
  }

 private:
  std::array<uint8_t, kWindow> ring_{};
  std::array<int, kBinCount> bins_{};
  int filled_ = 0, head_ = 0, clean_ = 0;
};

/// Dynamic rate control of the audio stream (AudioRateControl on macOS, RetroArch-style DRC).
/// ratio = output samples per input sample = base x (1 + clamp(gain x fill error, +-0.5 %)).
struct AudioRateControl {
  static constexpr double kMaxDeviation = 0.005;
  static constexpr double kGain = 0.005;
  static constexpr double kFillSmoothing = 0.02;
  double targetFill;  // samples
  double base = 1.0;
  double ratio = 1.0;
  std::optional<double> smoothedFill;

  explicit AudioRateControl(double target = 1600) : targetFill(target) {}

  void setFrameRate(double rate, double nominal = 1.0 / kNesFramePeriod) {
    if (rate > 0) base = nominal / rate;
  }
  void reset() { smoothedFill.reset(); ratio = base; }
  double update(double fill) {
    double f = smoothedFill ? *smoothedFill + (fill - *smoothedFill) * kFillSmoothing : fill;
    smoothedFill = f;
    double error = (targetFill - f) / targetFill;
    double adj = std::max(-kMaxDeviation, std::min(kMaxDeviation, error * kGain));
    ratio = base * (1 + adj);
    return ratio;
  }
};

}  // namespace rnl::interim
