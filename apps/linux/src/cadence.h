// Linux-specific part of display-locked pacing (the rest is the shared frontend core, rnf_*):
//  * Cadence: which refreshes start an emulated frame. Integer multiples of the NES period are the
//    core's rule (rnf_refreshes_per_frame: 60 Hz every refresh, 120 Hz every 2nd); Linux adds the
//    3:2 lock for 90 Hz panels (Steam Deck OLED): frames alternate 2 and 1 refreshes on screen.
//  * RefreshEstimator: the refresh interval from present-complete timestamps. Unlike the macOS
//    display-link callbacks (rnf_cadence), only presented pictures are timestamped here, so deltas
//    spanning several refreshes (3:2, k > 1, a missed refresh) are divided by their refresh count.
// Pure: no clocks, no I/O. Times in seconds.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "replaynes/frontend.h"

namespace rnl {

/// Exact NTSC frame period (RN_FPS_DEN / RN_FPS_NUM s, 60.0988 Hz).
inline double nesFramePeriod() { return rnf_frame_period(); }

struct Cadence {
  enum class Kind { unknown, locked, three_two, free };
  Kind kind = Kind::unknown;
  int k = 0;  // refreshes per frame when locked

  static Cadence classify(double refresh, double framePeriod = nesFramePeriod()) {
    Cadence c;
    if (!(refresh > 0)) return c;
    if (int k = rnf_refreshes_per_frame(refresh, framePeriod)) {
      c.kind = Kind::locked;
      c.k = k;
    } else if (std::fabs(1.5 * refresh - framePeriod) / framePeriod <= RNF_CADENCE_LOCK_TOLERANCE) {
      c.kind = Kind::three_two;
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
      default: return 1.0 / nesFramePeriod();
    }
  }

  const char* name() const {
    switch (kind) {
      case Kind::locked: return "locked";
      case Kind::three_two: return "3:2";
      case Kind::free: return "free";
      default: return "?";
    }
  }
};

/// Refresh interval estimate from display timestamps: the median of the last 15 plausible
/// deltas, refined by an EMA. Deltas spanning several refreshes are divided by their count.
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
    if (int(deltas_.size()) < kWindow) deltas_.push_back(one);
    else { deltas_[size_t(head_)] = one; head_ = (head_ + 1) % kWindow; }
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

}  // namespace rnl
