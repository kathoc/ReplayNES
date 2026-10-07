// ReplayNES for Linux / Steam Deck: the shared desktop frontend (apps/desktop/src/app.h) with the
// Vulkan presenter (vk_renderer.h), in-app updates through the Flatpak portal and "Add to Steam".
// SPDX-License-Identifier: GPL-2.0-or-later
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <sys/prctl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "app.h"
#include "platform/platform.h"
#include "steam_shortcut.h"
#include "update_service_flatpak.h"
#include "vk_renderer.h"

int main(int argc, char** argv) {
  using namespace rnl;
  if (int rc = 0; steam::runCli(argc, argv, &rc)) return rc;  // --add-to-steam [--dry-run] (no window)
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("ReplayNES %s\n", RNL_APP_VERSION);
    return 0;
  }
  PlatformHooks platform;
  platform.version = RNL_APP_VERSION;
  platform.executable = "replaynes-linux";
  platform.windowFlags = SDL_WINDOW_VULKAN;
  platform.makeRenderer = [](const AppOptions&) { return std::unique_ptr<Renderer>(new VkRenderer()); };
  platform.makeUpdates = [] { return std::unique_ptr<UpdateService>(new FlatpakUpdateService()); };
  platform.setupFrameThread = [] { prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0); };
  platform.sessionMode = []() -> std::string {
    if (gamingMode()) return "gamescope";
    const char* d = std::getenv("XDG_CURRENT_DESKTOP");
    return d ? d : "desktop";
  };
  AppOptions opt;
  if (!parseAppArgs(argc, argv, platform, &opt)) return 2;
  return runApp(opt, platform);
}
