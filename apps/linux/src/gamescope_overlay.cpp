// SPDX-License-Identifier: GPL-2.0-or-later
#include "gamescope_overlay.h"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <initializer_list>
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
  int (*closeDisplay)(Display*);
  bool load() {
    void* h = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!h) return false;
    openDisplay = reinterpret_cast<decltype(openDisplay)>(dlsym(h, "XOpenDisplay"));
    internAtom = reinterpret_cast<decltype(internAtom)>(dlsym(h, "XInternAtom"));
    defaultRootWindow = reinterpret_cast<decltype(defaultRootWindow)>(dlsym(h, "XDefaultRootWindow"));
    getWindowProperty = reinterpret_cast<decltype(getWindowProperty)>(dlsym(h, "XGetWindowProperty"));
    free = reinterpret_cast<decltype(free)>(dlsym(h, "XFree"));
    closeDisplay = reinterpret_cast<decltype(closeDisplay)>(dlsym(h, "XCloseDisplay"));
    return openDisplay && internAtom && defaultRootWindow && getWindowProperty && free && closeDisplay;
  }
};

constexpr unsigned long kSteamAppId = 769;  // gamescope's id of Steam's own UI

// The app id gamescope gives the input, or -1: the property is on the root of gamescope's first
// Xwayland only (Steam's, :0); a game launched by Steam runs on another one (:1) without it.
long focusedAppOf(X11& x, X11::Display* d, X11::Atom atom) {
  X11::Atom type = 0;
  int format = 0;
  unsigned long n = 0, after = 0;
  unsigned char* p = nullptr;
  long app = -1;
  // CARDINAL, format 32: Xlib returns it as an array of long.
  if (x.getWindowProperty(d, x.defaultRootWindow(d), atom, 0, 1, 0, 0 /* AnyPropertyType */, &type, &format, &n, &after, &p) == 0 &&
      p && format == 32 && n >= 1)
    app = *reinterpret_cast<long*>(p);
  if (p) x.free(p);
  return app;
}

void watch() {
  X11 x;
  if (!x.load()) return (void)std::fprintf(stderr, "gamescope: libX11 not loaded, the Steam menu does not pause\n");
  // Our display first; when its root has no GAMESCOPE_FOCUSED_APP, gamescope's first Xwayland (:0;
  // the Flatpak reaches it through the abstract socket: --share=network).
  X11::Display* d = nullptr;
  X11::Atom focusedApp = 0;
  for (const char* name : {static_cast<const char*>(nullptr), ":0"}) {
    X11::Display* c = x.openDisplay(name);
    if (!c) continue;
    const X11::Atom atom = x.internAtom(c, "GAMESCOPE_FOCUSED_APP", 0);
    if (focusedAppOf(x, c, atom) >= 0) {
      d = c;
      focusedApp = atom;
      std::fprintf(stderr, "gamescope: the Steam menu is read on %s\n", name ? name : "this display");
      break;
    }
    x.closeDisplay(c);
  }
  if (!d) return (void)std::fprintf(stderr, "gamescope: no display with GAMESCOPE_FOCUSED_APP, the Steam menu does not pause\n");
  for (;;) {
    shown.store(static_cast<unsigned long>(focusedAppOf(x, d, focusedApp)) == kSteamAppId, std::memory_order_relaxed);
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
