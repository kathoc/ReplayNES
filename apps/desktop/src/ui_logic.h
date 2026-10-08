// Controller conventions of the desktop UI as plain functions (unit-tested without SDL / ImGui):
// the seek bar's D-pad scrub steps, button prompt glyphs per controller family and the Menu
// pill's chord glyph. The menu tree and its navigation are the shared core's (rnf_menu).
//
//   L+R together = Quick Menu open / close (Esc on a keyboard), R alone = pause (the seek bar),
//   L alone = slow 1/2; in menus: A = confirm, B = back (on the top level: resume), L / R = the
//   previous / next page, X / Y = the hint bar's contextual actions, L2 / R2 = rewind / fast-forward.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "replaynes/frontend.h"

namespace rnl {

/// Frames one D-pad step moves the playhead on the seek bar, by how long the direction has been
/// held (hold for faster).
int scrubStepFrames(double heldSeconds);

/// The previous / next bookmark of the active take, or 5 s back / forward when there is none in
/// that direction (clamped to the recorded range).
uint64_t timelineJumpTarget(uint64_t frame, uint64_t takeLength, const std::vector<uint64_t>& bookmarks, int dir);

/// Button prompt glyph of a controller element ("face.south", "leftShoulder", "menu", ...) for the
/// family: Steam Deck / Xbox A B X Y, PlayStation ✕ ○ □ △, Nintendo by position (south = B).
std::string padGlyph(rnf_controller_family family, const char* element);

/// The Quick Menu chord on the Menu pill: "L+R" (Steam Deck, Xbox, Nintendo, generic), "L1+R1"
/// (PlayStation), "Esc" with no controller connected.
std::string menuChordGlyph(rnf_controller_family family, bool controllerConnected);

/// "A = resume" on the paused seek bar, as a tap: the confirm button pressed while paused still
/// reaches the game (held for a frame advance, e.g. hold B and step with the D-pad), and resumes on
/// its release only when nothing else was pressed or stepped while it was held.
class ConfirmTap {
 public:
  /// Confirm pressed while paused (call after cancel() for this press).
  void press(const std::string& id) { held_[id] = true; }
  /// Another input pressed / a frame step: the confirm buttons held now are no longer taps.
  void cancel() {
    for (auto& h : held_) h.second = false;
  }
  /// Released: true = it was a tap (resume).
  bool release(const std::string& id) {
    auto it = held_.find(id);
    if (it == held_.end()) return false;
    bool tap = it->second;
    held_.erase(it);
    return tap;
  }
  void clear() { held_.clear(); }

 private:
  std::map<std::string, bool> held_;
};

}  // namespace rnl
