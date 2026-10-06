// INTERIM FRONTEND LOGIC (Steam Deck plan, Step 2 skeleton).
// Minimal C++ port of apps/macos/Sources/Core/PlaybackLogic.swift (record/replay toggle, slow
// toggle, paused D-pad stepping, fast-forward over the recorded take) and of the default bindings
// in apps/macos/Sources/Core/InputCatalog.swift (controller part; keyboard ids are SDL scancodes
// here). To be replaced by the shared frontend core (frontend/, plan Step 3).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "replaynes/replaynes.h"

namespace rnl::interim {

enum class SlowRate : int { normal = 1, half = 2, quarter = 4 };
inline SlowRate toggled(SlowRate s) { return s == SlowRate::normal ? SlowRate::half : SlowRate::normal; }

/// RecordToggle.plan (macOS): what "record / playback" does from the current state.
struct RecordTogglePlan {
  bool record;
  bool seekToStart;
  bool play;
};
inline RecordTogglePlan planRecordToggle(bool recording, uint64_t frame, uint64_t takeLength) {
  if (recording) {
    if (takeLength == 0) return {false, false, false};  // nothing recorded yet: stay recording
    return {false, frame >= takeLength, true};
  }
  return {true, false, false};
}

/// Paused D-pad stepping with key repeat (StepRepeater): 0.3 s delay, then ~20 steps/s.
struct StepRepeater {
  static constexpr int kInitialDelayTicks = 18;
  static constexpr int kIntervalTicks = 3;
  int direction = 0;
  int heldTicks = 0;
  int press(int dir) { direction = dir; heldTicks = 0; return dir; }
  void release(int dir) { if (dir == direction) { direction = 0; heldTicks = 0; } }
  int tick() {
    if (direction == 0) return 0;
    heldTicks += 1;
    if (heldTicks >= kInitialDelayTicks && (heldTicks - kInitialDelayTicks) % kIntervalTicks == 0) return direction;
    return 0;
  }
  void reset() { direction = 0; heldTicks = 0; }
};

/// Fast-forward = replay the recorded take only (FastForwardSession): REPLAY while held, back to
/// RECORD afterwards if that was the mode.
struct FastForward {
  bool active = false;
  bool restoreRecord = false;
  rn_status begin(rn_session* s) {
    if (active) return RN_OK;
    active = true;
    restoreRecord = rn_get_mode(s) == RN_MODE_RECORD;
    return restoreRecord ? rn_set_mode(s, RN_MODE_REPLAY) : RN_OK;
  }
  /// Steps up to n recorded frames. Returns frames done; *atEnd when the take end was reached.
  int step(rn_session* s, int n, bool* atEnd, rn_status* err) {
    int done = 0;
    *atEnd = false;
    *err = RN_OK;
    for (int i = 0; i < n; ++i) {
      if (rn_frame(s) >= rn_take_length(s)) { *atEnd = true; return done; }
      uint64_t before = rn_frame(s);
      rn_step_info info{};
      rn_status st = rn_step(s, 0, 0, 0, &info);
      if (st != RN_OK) { *err = st; return done; }
      if (info.end_of_take && info.frame == before) { *atEnd = true; return done; }
      done += 1;
    }
    *atEnd = rn_frame(s) >= rn_take_length(s);
    return done;
  }
  rn_status end(rn_session* s) {
    if (!active) return RN_OK;
    active = false;
    if (restoreRecord) { restoreRecord = false; return rn_set_mode(s, RN_MODE_RECORD); }
    return RN_OK;
  }
};

/// Controller defaults (InputCatalog.defaultBindings, layout 3): face buttons by POSITION
/// (east = A, south = B, north = turbo A, west = turbo B; on the Steam Deck / Xbox layout that is
/// B = NES A, A = NES B), menu = START, options/view = SELECT. Hotkeys: R2 hold = rewind,
/// L2 hold = fast-forward, L = slow 1/2 toggle, R = pause/play; D-pad left/right steps while paused
/// (frontend rule).
inline std::vector<std::pair<std::string, std::string>> defaultControllerBindings() {
  std::vector<std::pair<std::string, std::string>> b;
  for (int slot = 0; slot < 2; ++slot) {
    std::string g = "gc" + std::to_string(slot) + ":";
    std::string p = slot == 0 ? "p1." : "p2.";
    for (const char* d : {"up", "down", "left", "right"}) {
      b.push_back({g + "dpad." + d, p + d});
      b.push_back({g + "lstick." + d, p + d});
    }
    b.push_back({g + "face.east", p + "a"});
    b.push_back({g + "face.south", p + "b"});
    b.push_back({g + "face.north", p + "turbo_a"});
    b.push_back({g + "face.west", p + "turbo_b"});
    b.push_back({g + "menu", p + "start"});
    b.push_back({g + "options", p + "select"});
  }
  b.push_back({"gc0:rightTrigger", "hk.rewind"});
  b.push_back({"gc0:leftTrigger", "hk.fast_forward"});
  b.push_back({"gc0:leftShoulder", "hk.slow"});
  b.push_back({"gc0:rightShoulder", "hk.pause"});
  return b;
}

}  // namespace rnl::interim
