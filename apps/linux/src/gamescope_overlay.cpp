// SPDX-License-Identifier: GPL-2.0-or-later
#include "gamescope_overlay.h"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace rnl::gamescope {
namespace {

std::atomic<bool> shown{false};

// libX11 at run time (as SDL loads it): no build dependency, and in the Flatpak runtime.
struct X11 {
  using Display = void;
  using Atom = unsigned long;
  using Window = unsigned long;
  Display* (*openDisplay)(const char*);
  Atom (*internAtom)(Display*, const char*, int);
  Window (*defaultRootWindow)(Display*);
  int (*getWindowProperty)(Display*, Window, Atom, long, long, int, Atom, Atom*, int*, unsigned long*, unsigned long*,
                           unsigned char**);
  int (*free)(void*);
  bool load() {
    void* h = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!h) return false;
    openDisplay = reinterpret_cast<decltype(openDisplay)>(dlsym(h, "XOpenDisplay"));
    internAtom = reinterpret_cast<decltype(internAtom)>(dlsym(h, "XInternAtom"));
    defaultRootWindow = reinterpret_cast<decltype(defaultRootWindow)>(dlsym(h, "XDefaultRootWindow"));
    getWindowProperty = reinterpret_cast<decltype(getWindowProperty)>(dlsym(h, "XGetWindowProperty"));
    free = reinterpret_cast<decltype(free)>(dlsym(h, "XFree"));
    return openDisplay && internAtom && defaultRootWindow && getWindowProperty && free;
  }
};

constexpr unsigned long kSteamAppId = 769;  // gamescope's id of Steam's own UI

void watch() {
  X11 x;
  if (!x.load()) return (void)std::fprintf(stderr, "gamescope: libX11 not loaded, the Steam menu does not pause\n");
  X11::Display* d = x.openDisplay(nullptr);
  if (!d) return (void)std::fprintf(stderr, "gamescope: no X display, the Steam menu does not pause\n");
  const X11::Window root = x.defaultRootWindow(d);
  const X11::Atom focusedApp = x.internAtom(d, "GAMESCOPE_FOCUSED_APP", 0);
  for (;;) {
    X11::Atom type = 0;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* p = nullptr;
    bool steam = false;
    // CARDINAL, format 32: Xlib returns it as an array of long.
    if (x.getWindowProperty(d, root, focusedApp, 0, 1, 0, 0 /* AnyPropertyType */, &type, &format, &n, &after, &p) == 0 &&
        p && format == 32 && n >= 1)
      steam = static_cast<unsigned long>(*reinterpret_cast<long*>(p)) == kSteamAppId;
    if (p) x.free(p);
    shown.store(steam, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

}  // namespace

bool steamOverlayShown() {
  static std::once_flag started;
  std::call_once(started, [] { std::thread(watch).detach(); });
  return shown.load(std::memory_order_relaxed);
}

}  // namespace rnl::gamescope
