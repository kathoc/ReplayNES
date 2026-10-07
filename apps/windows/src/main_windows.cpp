// ReplayNES for Windows: the shared desktop frontend (apps/desktop/src/app.h) with the Direct3D 11
// presenter (d3d11_renderer.h); SDL3 (third_party/SDL, linked statically) for the window, WASAPI
// audio and gamepads (XInput / raw input / GameInput / HIDAPI). A WIN32-subsystem program: output
// goes to the console it was started from (if any) or to --log FILE.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "app.h"
#include "d3d11_renderer.h"

namespace {

/// Output of this GUI program: --log FILE, else the console of the shell that started it.
void setupOutput(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--log") == 0) {
      std::freopen(argv[i + 1], "w", stderr);
      std::freopen(argv[i + 1], "a", stdout);
      setvbuf(stderr, nullptr, _IONBF, 0);
      setvbuf(stdout, nullptr, _IONBF, 0);
      return;
    }
  }
  // Already redirected by the parent (a pipe / file): keep that.
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN) return;
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
  }
}

/// Removes "--log FILE" (handled above) from the arguments the app parses.
void dropLogArgument(int* argc, char** argv) {
  for (int i = 1; i + 1 < *argc; ++i) {
    if (std::strcmp(argv[i], "--log") == 0) {
      for (int j = i; j + 2 <= *argc; ++j) argv[j] = argv[j + 2];
      *argc -= 2;
      return;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace rnl;
  setupOutput(argc, argv);
  dropLogArgument(&argc, argv);
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("ReplayNES %s\n", RNL_APP_VERSION);
    return 0;
  }
  PlatformHooks platform;
  platform.version = RNL_APP_VERSION;
  platform.executable = "ReplayNES.exe";
  platform.makeRenderer = [](const AppOptions& o) { return std::unique_ptr<Renderer>(new D3D11Renderer(o.vrr)); };
  platform.setupFrameThread = [] {
    // Multimedia Class Scheduler "Games": the frame loop gets priority boosts and timer precision.
    DWORD task = 0;
    if (!AvSetMmThreadCharacteristicsW(L"Games", &task)) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  };
  platform.sessionMode = [] { return std::string("windows"); };
  AppOptions opt;
  if (!parseAppArgs(argc, argv, platform, &opt)) return 2;
  return runApp(opt, platform);
}
