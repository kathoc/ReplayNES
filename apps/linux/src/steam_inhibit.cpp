// SPDX-License-Identifier: GPL-2.0-or-later
#include "steam_inhibit.h"

#include <cstdio>

#include "replaynes/product.h"
#ifdef RNL_HAVE_GIO
#include <gio/gio.h>
#endif

namespace steam {

bool inhibitScreensaver() {
#ifdef RNL_HAVE_GIO
  GError* e = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &e);
  if (!bus) {
    std::fprintf(stderr, "screensaver: no session bus (%s)\n", e ? e->message : "?");
    if (e) g_error_free(e);
    return false;
  }
  GVariant* r = g_dbus_connection_call_sync(bus, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
                                            "org.freedesktop.ScreenSaver", "Inhibit",
                                            g_variant_new("(ss)", RN_PRODUCT_NAME, "Playing"), G_VARIANT_TYPE("(u)"),
                                            G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &e);
  if (!r) {
    std::fprintf(stderr, "screensaver: Steam's dimming not held off (%s)\n", e ? e->message : "?");
    if (e) g_error_free(e);
    g_object_unref(bus);
    return false;
  }
  guint32 cookie = 0;
  g_variant_get(r, "(u)", &cookie);
  g_variant_unref(r);
  std::fprintf(stderr, "screensaver: Steam's dimming held off (cookie %u)\n", unsigned(cookie));
  return true;  // `bus` stays open: the inhibition lasts while it does (until the process exits)
#else
  return false;
#endif
}

}  // namespace steam
