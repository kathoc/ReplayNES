// Display-locked frame scheduling for the Vulkan present loop (Linux-specific part).
//
// A waiter thread reports when each present reached the screen (VK_KHR_present_wait); this class
// keeps a vblank grid (refresh estimate + phase) from those timestamps and picks the vblank each
// next frame is aimed at (target), following the cadence (Cadence, cadence.h: 60 Hz every refresh,
// 120 Hz every 2nd, 90 Hz alternating 2/1, other rates free). The caller samples input at
// target - lead (rnf_input_deadline; the lead may exceed a refresh: frames overlap) and
// reports whether each picture reached the screen at its target (a miss raises the lead).
// A FIFO backlog (pictures queued behind a late one, every later picture a refresh late) is
// drained by moving the targets one refresh later once (BacklogDrain on macOS).
// Present-complete timestamps are taken when vkWaitForPresentKHR returns, i.e. slightly after the
// real flip; the phase tracks the earliest of them (the delays only ever add).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>
#include <cstdint>

#include "cadence.h"

namespace rnl {

class DisplayScheduler {
 public:
  void seedRefresh(double r) { est_.seed(r); }
  double refresh() const { return est_.refresh; }
  Cadence cadence() const { return Cadence::classify(est_.refresh); }
  bool hasGrid() const { return hasAnchor_ && est_.refresh > 0; }
  uint64_t skippedRefreshes() const { return skipped_; }

  /// The present aimed at `target` reached the screen at t. Returns true if it missed its target.
  bool observeDisplayed(double t, double target) {
    if (lastStamp_ > 0) {
      double before = est_.refresh;
      est_.observeDelta(t - lastStamp_);
      // Several deltas in a row that fit no multiple of the estimate: the seed (display mode) was
      // wrong, e.g. the compositor runs another rate. Start over from the raw deltas.
      if (before > 0 && est_.refresh == before && !fits(t - lastStamp_, before)) {
        if (++rejected_ >= 30) { est_ = RefreshEstimator(); rejected_ = 0; }
      } else {
        rejected_ = 0;
      }
    }
    lastStamp_ = t;
    double r = est_.refresh;
    if (r > 0) {
      if (!hasAnchor_) {
        anchor_ = t;
        hasAnchor_ = true;
      } else {
        double pred = snap(t);
        double err = t - pred;
        anchor_ = pred + (err < 0 ? err : err * 0.05);
      }
    }
    bool late = target > 0 && r > 0 && t > target + r / 2;
    // Backlog: two pictures in a row late that were both aimed after the last drain.
    if (late && target > drainGuard_) {
      if (++lateRun_ >= 2) {
        lastTarget_ += r;
        drainGuard_ = lastTarget_;
        lateRun_ = 0;
        drains_ += 1;
      }
    } else if (!late) {
      lateRun_ = 0;
    }
    return late;
  }

  uint64_t backlogDrains() const { return drains_; }

  /// Target vblank of the next frame, with input sampled `lead` seconds before it: the cadence's
  /// next vblank after the previous target, moved on while now + lead does not fit.
  double nextTarget(double now, double lead) {
    double r = est_.refresh;
    if (!(r > 0) || !hasAnchor_) {
      lastTarget_ = now + lead + nesFramePeriod();
      return lastTarget_;
    }
    Cadence c = Cadence::classify(r);
    double t;
    if (lastTarget_ <= 0 || lastTarget_ < now - 4 * nesFramePeriod()) {
      t = snap(now + lead + r);
      content_ = t;
    } else if (c.kind == Cadence::Kind::locked) {
      t = snap(lastTarget_ + c.k * r);
    } else if (c.kind == Cadence::Kind::three_two) {
      pattern_ ^= 1;
      t = snap(lastTarget_ + (pattern_ ? 2 : 1) * r);
    } else {
      content_ += nesFramePeriod();
      t = snap(content_);
      if (t <= lastTarget_) t = snap(lastTarget_ + r);
      if (std::fabs(t - content_) > 2 * nesFramePeriod()) content_ = t;
    }
    while (t - lead < now + 0.0002) {
      t += r;
      skipped_ += 1;
    }
    lastTarget_ = t;
    return t;
  }

 private:
  double snap(double x) const {
    double r = est_.refresh;
    return anchor_ + std::round((x - anchor_) / r) * r;
  }
  static bool fits(double d, double r) {
    double n = std::round(d / r);
    return n >= 1 && n <= 8 && std::fabs(d - n * r) <= 0.25 * r;
  }

  RefreshEstimator est_;
  double anchor_ = 0;
  bool hasAnchor_ = false;
  double lastStamp_ = 0;
  double lastTarget_ = 0;
  double drainGuard_ = 0;
  int lateRun_ = 0;
  uint64_t drains_ = 0;
  double content_ = 0;
  int pattern_ = 0;
  int rejected_ = 0;
  uint64_t skipped_ = 0;
};

}  // namespace rnl
