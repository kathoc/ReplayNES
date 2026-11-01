// The Steam menu over the game in gamescope (Steam Deck game mode): gamescope draws it without moving
// the game window's focus, so SDL sends no focus event; the root window property
// GAMESCOPE_FOCUSED_APP names the app in front instead, 769 for Steam's own UI
// (docs/design/STEAM_OVERLAY_PAUSE.md).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace rnl::gamescope {

/// Steam's UI is in front of the game: the last answer of a thread that reads the property every
/// 250 ms over its own X connection (started by the first call). False when X cannot be reached.
bool steamOverlayShown();

}  // namespace rnl::gamescope
