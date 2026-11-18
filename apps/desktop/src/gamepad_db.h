// SDL_GameControllerDB built into the app, added to SDL before the controllers are listed
// (docs/design/CONTROLLERS.md; the table: apps/desktop/gamepad_db.cmake).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

namespace rnl {

struct GamepadDbResult {
  int builtIn = 0;     // mappings the built-in table added that SDL did not know (-1: it refused the table)
  int builtInLines = 0;
  int user = 0;        // from the user's file (0: none, -1: unreadable)
  std::string platform, commit;
};

/// Adds the built-in table, then `userFile` when it exists (a newer table, or one line for one controller).
/// SDL's gamepad subsystem must be initialised; call it before the controllers are listed. Logs one line.
GamepadDbResult addGamepadMappings(const std::string& userFile);

}  // namespace rnl
