// SDL_GameControllerDB built into the app (docs/design/CONTROLLERS.md).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gamepad_db.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <filesystem>

#include "gamepad_db.inc"  // generated: kGamepadDb, kGamepadDbLines, kGamepadDbPlatform, kGamepadDbCommit

namespace rnl {

GamepadDbResult addGamepadMappings(const std::string& userFile) {
  GamepadDbResult r;
  r.platform = kGamepadDbPlatform;
  r.commit = kGamepadDbCommit;
  r.builtInLines = kGamepadDbLines;
  if (SDL_IOStream* io = SDL_IOFromConstMem(kGamepadDb, sizeof kGamepadDb))
    r.builtIn = SDL_AddGamepadMappingsFromIO(io, true);
  else
    r.builtIn = -1;
  std::error_code ec;
  if (!userFile.empty() && std::filesystem::exists(std::filesystem::u8path(userFile), ec))
    r.user = SDL_AddGamepadMappingsFromFile(userFile.c_str());
  std::fprintf(stderr, "controllers: SDL_GameControllerDB %.12s (%s): %d new of %d mappings (the rest SDL knew)%s%s\n", r.commit.c_str(),
               r.platform.c_str(), r.builtIn, r.builtInLines, r.user ? ", user file: " : "",
               r.user ? (r.user < 0 ? SDL_GetError() : (std::to_string(r.user) + " new").c_str()) : "");
  return r;
}

}  // namespace rnl
