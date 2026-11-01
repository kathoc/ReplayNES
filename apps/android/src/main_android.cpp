// ReplayNES for Android: the shared desktop frontend (apps/desktop/src/app.h) with the Vulkan presenter
// of apps/linux (vk_renderer.h), full screen. SDLActivity calls SDL_main in libmain.so.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "replaynes/product.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <android/log.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "app.h"
#include "vk_renderer.h"

#ifdef RN_PRODUCT_HAS_STARTUP
extern "C" void rn_product_startup(void);
#endif

namespace {
// stdout / stderr into logcat (tag = the product): the app logs with fprintf(stderr), as on the
// desktop, and Android drops both streams. `adb logcat -s ReplayArcade` reads them.
void logToLogcat() {
  static int fds[2];
  if (pipe(fds) != 0) return;
  dup2(fds[1], STDOUT_FILENO);
  dup2(fds[1], STDERR_FILENO);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  std::thread([] {
    std::string line;
    char buf[512];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;)
      for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] != '\n') { line += buf[i]; continue; }
        __android_log_write(ANDROID_LOG_INFO, RN_PRODUCT_NAME, line.c_str());
        line.clear();
      }
  }).detach();
}
}  // namespace

int main(int argc, char** argv) {
  using namespace rnl;
  logToLogcat();
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
