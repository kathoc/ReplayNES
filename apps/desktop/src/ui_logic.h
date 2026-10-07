// Controller conventions of the Linux UI as plain functions (unit-tested without SDL / ImGui):
// which page B / Menu (≡) / View (⧉) / L1 / R1 lead to, the timeline's D-pad scrub steps and
// L1 / R1 jumps, and the button prompt glyphs per controller family.
//
//   A = confirm, B = back (a page returns to the hub; on the hub B resumes play), Menu (≡) =
//   hub open / close (no session: Library <-> Settings), View (⧉) = Controls Guide on / off,
//   L1 / R1 = Settings tabs (other pages: the next page), D-pad / left stick = focus.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "replaynes/frontend.h"

namespace rnl {

/// Menu pages. `playback` is the hub (the paused overlay: game, timeline, transport, hub buttons).
enum class MenuPage { playback, takes, bookmarks, practice, library, settings, guide };

enum class MenuCommand { back, menuButton, viewButton };

struct MenuTransition {
  MenuPage page = MenuPage::playback;
  bool closeAndResume = false;  // close the menu and resume play (session only)
};

/// Where B / Menu / View lead from `current`. `previous` = the page shown before the guide (View
/// toggles back to it).
MenuTransition menuTransition(MenuPage current, MenuCommand cmd, bool hasSession, MenuPage previous);

/// L1 / R1 on a page that has no tabs of its own: the previous / next page among Practice, Takes,
/// Bookmarks and the Guide (`current` unchanged elsewhere).
MenuPage cyclePage(MenuPage current, int dir);

/// Frames one D-pad step moves the playhead on the focused timeline, by how long the direction
/// has been held (hold for faster).
int scrubStepFrames(double heldSeconds);

/// L1 / R1 on the focused timeline: the previous / next bookmark of the active take, or 5 s
/// back / forward when there is none in that direction (clamped to the recorded range).
uint64_t timelineJumpTarget(uint64_t frame, uint64_t takeLength, const std::vector<uint64_t>& bookmarks, int dir);

/// Button prompt glyph of a controller element ("face.south", "leftShoulder", "menu", ...) for the
/// family: Steam Deck / Xbox A B X Y, PlayStation ✕ ○ □ △, Nintendo by position (south = B).
std::string padGlyph(rnf_controller_family family, const char* element);

}  // namespace rnl
