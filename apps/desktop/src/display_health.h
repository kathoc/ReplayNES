// Display path health of the desktop frontend (docs/FRAME_PACING.md "Display watchdog"): the game
// and audio never wait for the display, so a display path that stops delivering pictures (a lost
// device / surface, a failing submit or present, a stale CRT picture) freezes the picture while
// play goes on. The presenters (Vulkan, Direct3D 11) log every failure with its code through
// DisplayErrors and recover from what they can see themselves; the frame loop (app.cpp) runs
// DisplayWatchdog over what actually reached the screen and asks the renderer for RECOVER /
// RESTART (Renderer::recoverDisplay). No SDL / graphics here (unit-tested headlessly).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "replaynes/frontend.h"

namespace rnl {

/// Rate limit of the display failure log: the first 5 failures, then every 300th (a failure that
/// repeats every frame must not flood the log, but stays visible).
inline bool displayLogDue(int count) { return count <= 5 || count % 300 == 0; }

/// Failures of display calls (acquire, submit, present, fence waits, device / swap chain creation).
class DisplayErrors {
 public:
  /// Counts a failure of `what` with the platform's result code (`name`: its symbolic name, may be
  /// empty) and logs it when due. Returns true when it was logged.
  bool note(const std::string& what, long code, const std::string& name = std::string()) {
    count_ += 1;
    last_ = what;
    lastCode_ = code;
    lastName_ = name;
    if (!displayLogDue(count_)) return false;
    std::fprintf(stderr, "ReplayNES: display: %s failed: %s (%ld / 0x%08lx), %d display failures so far\n", what.c_str(),
                 name.empty() ? "?" : name.c_str(), code, (unsigned long)uint32_t(code), count_);
    return true;
  }
  int count() const { return count_; }
  /// "N display failures, last: what NAME (code)" or "no display failures".
  std::string summary() const {
    if (count_ == 0) return "no display failures";
    char b[256];
    std::snprintf(b, sizeof b, "%d display failures, last: %s %s (%ld)", count_, last_.c_str(), lastName_.empty() ? "?" : lastName_.c_str(),
                  lastCode_);
    return b;
  }

 private:
  int count_ = 0;
  std::string last_, lastName_;
  long lastCode_ = 0;
};

/// The frame loop's side of rnf_display_watchdog (frontend.h).
class DisplayWatchdog {
 public:
  enum class Action { none, recover, restart };
  DisplayWatchdog() : w_(rnf_display_watchdog_new()) {}
  ~DisplayWatchdog() { rnf_display_watchdog_free(w_); }
  DisplayWatchdog(const DisplayWatchdog&) = delete;
  DisplayWatchdog& operator=(const DisplayWatchdog&) = delete;

  /// Once per frame after the present. `visible`: the viewport can show pictures (window shown,
  /// not minimised / occluded, game area on screen) - otherwise nothing is expected on screen and
  /// the wait starts over. `newPicture`: a new picture was published this frame. `shown`: this
  /// frame's present succeeded and the CRT build of its picture did not fail (the picture in the
  /// renderer, the newest published one, is on screen).
  Action frame(double now, bool visible, bool newPicture, bool shown) {
    if (!visible) {
      rnf_display_watchdog_reset(w_);
      return Action::none;
    }
    if (newPicture) rnf_display_watchdog_emulated(w_, now);
    if (shown) rnf_display_watchdog_presented(w_, now);
    switch (rnf_display_watchdog_check(w_, now)) {
      case RNF_WATCHDOG_RECOVER: return Action::recover;
      case RNF_WATCHDOG_RESTART: return Action::restart;
      default: return Action::none;
    }
  }
  /// Seconds the oldest unshown picture has waited.
  double waiting(double now) const { return rnf_display_watchdog_waiting(w_, now); }
  int actions() const { return rnf_display_watchdog_recoveries(w_); }

 private:
  rnf_display_watchdog* w_;
};

}  // namespace rnl
