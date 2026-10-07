// The desktop frontend application (SDL3 window + gamepads + audio, Dear ImGui UI, display-locked
// frame loop on the engine C API and the shared frontend core), shared by Linux and Windows. The
// platform's main() fills PlatformHooks (renderer, in-app updates, thread setup) and calls runApp.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rnl {

class Renderer;
class UpdateService;

struct AppOptions {
  std::string rom;
  double perfSeconds = 0;
  double warmup = 8;
  std::string statsLog, frameLog, sessionRoot, libraryRoot, script, label = "run";
  int fullscreen = -1;  // -1 = auto (gamescope: on)
  bool injectInput = false;
  bool resume = true;
  int flash = -1;  // -1 = the saved setting
  // Measurement overrides (not saved): CRT display on, its knobs, integer scaling.
  int crt = -1;  // --crt 1 / --no-crt 0 / -1 = the saved setting
  int crtMaxWidth = 1600;
  bool crtAdaptive = true;
  bool crtBuildAhead = true;
  int integerScale = -1;  // -1 = the saved setting
  /// Windows: present without vsync with tearing allowed in full screen (variable refresh
  /// displays) and pace emulation on the host clock at the NES rate. Ignored elsewhere.
  bool vrr = false;
  std::string lang;               // --lang ja|en (else REPLAYNES_LANG, else the system's)
  std::vector<std::string> argv;  // as started (restart into an update)
};

struct PlatformHooks {
  const char* version = "";
  const char* executable = "replaynes";  // for --help
  /// Extra SDL_CreateWindow flags (SDL_WINDOW_VULKAN on Linux).
  SDL_WindowFlags windowFlags = 0;
  std::function<std::unique_ptr<Renderer>(const AppOptions&)> makeRenderer;
  /// In-app updates; empty: the platform has none (the UI shows only the version).
  std::function<std::unique_ptr<UpdateService>()> makeUpdates;
  /// Once on the frame-loop thread before the loop (timer slack, MMCSS, timer resolution ...).
  std::function<void()> setupFrameThread;
  /// "mode" of the --perf-seconds summary (gamescope / the desktop).
  std::function<std::string()> sessionMode;
};

/// Parses the shared command line (unknown options: usage + false).
bool parseAppArgs(int argc, char** argv, const PlatformHooks& platform, AppOptions* o);
void printUsage(const PlatformHooks& platform);
int runApp(const AppOptions& opt, const PlatformHooks& platform);

}  // namespace rnl
