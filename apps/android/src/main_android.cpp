// ReplayNES for Android: the shared desktop frontend (apps/desktop/src/app.h) with the Vulkan presenter
// of apps/linux (vk_renderer.h), full screen. SDLActivity calls SDL_main in libmain.so.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "replaynes/product.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <memory>
#include <string>

#include "app.h"
#include "vk_renderer.h"

#ifdef RN_PRODUCT_HAS_STARTUP
extern "C" void rn_product_startup(void);
#endif

int main(int argc, char** argv) {
  using namespace rnl;
#ifdef RN_PRODUCT_HAS_STARTUP
  rn_product_startup();
#endif
  PlatformHooks platform;
  platform.version = RNL_APP_VERSION;
  platform.executable = RN_PRODUCT_EXECUTABLE;
  platform.windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN;
  platform.makeRenderer = [](const AppOptions&) { return std::unique_ptr<Renderer>(new VkRenderer()); };
  platform.sessionMode = [] { return std::string("android"); };
  AppOptions opt;
  if (!parseAppArgs(argc, argv, platform, &opt)) return 2;
  return runApp(opt, platform);
}
