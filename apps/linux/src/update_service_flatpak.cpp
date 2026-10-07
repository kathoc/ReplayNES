// SPDX-License-Identifier: GPL-2.0-or-later
#include "update_service_flatpak.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

#ifdef RNL_HAVE_GIO
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#endif

extern char** environ;

namespace rnl {

bool inFlatpakSandbox() { return ::access("/.flatpak-info", F_OK) == 0; }

#ifdef RNL_HAVE_GIO

namespace {
constexpr const char* kPortalName = "org.freedesktop.portal.Flatpak";
constexpr const char* kPortalPath = "/org/freedesktop/portal/Flatpak";
constexpr const char* kPortalIface = "org.freedesktop.portal.Flatpak";
constexpr const char* kMonitorIface = "org.freedesktop.portal.Flatpak.UpdateMonitor";
// Spawn flags (org.freedesktop.portal.Flatpak.xml).
constexpr guint32 kSpawnLatestVersion = 2, kSpawnSandbox = 4, kSpawnNoNetwork = 8, kSpawnWatchBus = 16;

std::string str(GVariant* dict, const char* key) {
  const char* s = nullptr;
  if (g_variant_lookup(dict, key, "&s", &s) && s) return s;
  return {};
}
guint32 u32(GVariant* dict, const char* key) {
  guint32 v = 0;
  g_variant_lookup(dict, key, "u", &v);
  return v;
}
/// D-Bus error name + message of a failed call.
std::pair<std::string, std::string> errorParts(GError* e) {
  if (!e) return {"", ""};
  std::string name;
  if (g_dbus_error_is_remote_error(e)) {
    gchar* n = g_dbus_error_get_remote_error(e);
    if (n) name = n;
    g_free(n);
    g_dbus_error_strip_remote_error(e);
  }
  return {name, e->message ? e->message : ""};
}
}  // namespace

struct FlatpakUpdateService::Impl {
  GMainContext* ctx = nullptr;
  GMainLoop* loop = nullptr;
  std::thread thread;
  mutable std::mutex m;
  UpdateModel model;
  // Service thread only.
  GDBusConnection* bus = nullptr;
  bool portalOK = false;
  bool autoCheck = true;
  bool installing = false;  // Update() in flight
  std::string monitorPath;
  guint monitorSub = 0;
  int tokenSeq = 0;
  // Restart (SpawnExited).
  guint exitSub = 0;
  guint32 childPid = 0;
  std::promise<int>* exitPromise = nullptr;

  template <class F>
  void withModel(F f) {
    std::lock_guard<std::mutex> lk(m);
    f(model);
  }

  void post(std::function<void()> f) {
    auto* fn = new std::function<void()>(std::move(f));
    g_main_context_invoke_full(
        ctx, G_PRIORITY_DEFAULT,
        [](gpointer p) -> gboolean {
          (*static_cast<std::function<void()>*>(p))();
          return G_SOURCE_REMOVE;
        },
        fn, [](gpointer p) { delete static_cast<std::function<void()>*>(p); });
  }

  void run(bool autoCheckAtStart) {
    g_main_context_push_thread_default(ctx);
    autoCheck = autoCheckAtStart;
    connect();
    g_main_loop_run(loop);
    closeMonitor();
    if (exitSub) g_dbus_connection_signal_unsubscribe(bus, exitSub);
    if (bus) {
      g_dbus_connection_flush_sync(bus, nullptr, nullptr);
      g_object_unref(bus);
    }
    bus = nullptr;
    g_main_context_pop_thread_default(ctx);
  }

  void connect() {
    if (!inFlatpakSandbox()) {
      withModel([](UpdateModel& md) { md.setUnsupported(UpdateUnavailable::notFlatpak); });
      return;
    }
    GError* e = nullptr;
    bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &e);
    if (!bus) {
      std::fprintf(stderr, "updates: no session bus: %s\n", e ? e->message : "?");
      g_clear_error(&e);
      withModel([](UpdateModel& md) { md.setUnsupported(UpdateUnavailable::noPortal); });
      return;
    }
    GVariant* r = g_dbus_connection_call_sync(bus, kPortalName, kPortalPath, "org.freedesktop.DBus.Properties", "Get",
                                              g_variant_new("(ss)", kPortalIface, "version"), G_VARIANT_TYPE("(v)"),
                                              G_DBUS_CALL_FLAGS_NONE, 10000, nullptr, &e);
    guint32 version = 0;
    if (r) {
      GVariant* v = nullptr;
      g_variant_get(r, "(v)", &v);
      if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) version = g_variant_get_uint32(v);
      if (v) g_variant_unref(v);
      g_variant_unref(r);
    } else {
      std::fprintf(stderr, "updates: no Flatpak portal: %s\n", e ? e->message : "?");
      g_clear_error(&e);
    }
    if (version < 2) {  // CreateUpdateMonitor: version 2 (flatpak 1.5)
      withModel([](UpdateModel& md) { md.setUnsupported(UpdateUnavailable::noPortal); });
      return;
    }
    portalOK = true;
    withModel([](UpdateModel& md) { md.portalReady(); });
    if (autoCheck) openMonitor();
  }

  bool openMonitor() {
    if (!monitorPath.empty()) return true;
    if (!portalOK) return false;
    char token[64];
    std::snprintf(token, sizeof token, "replaynes%d_%d", int(::getpid()), ++tokenSeq);
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "handle_token", g_variant_new_string(token));
    GError* e = nullptr;
    GVariant* r = g_dbus_connection_call_sync(bus, kPortalName, kPortalPath, kPortalIface, "CreateUpdateMonitor",
                                              g_variant_new("(a{sv})", &opts), G_VARIANT_TYPE("(o)"),
                                              G_DBUS_CALL_FLAGS_NONE, 10000, nullptr, &e);
    if (!r) {
      auto [name, msg] = errorParts(e);
      std::fprintf(stderr, "updates: CreateUpdateMonitor failed: %s %s\n", name.c_str(), msg.c_str());
      g_clear_error(&e);
      withModel([&](UpdateModel& md) { md.callFailed(name, msg); });
      return false;
    }
    const char* path = nullptr;
    g_variant_get(r, "(&o)", &path);
    monitorPath = path ? path : "";
    g_variant_unref(r);
    monitorSub = g_dbus_connection_signal_subscribe(
        bus, nullptr, kMonitorIface, nullptr, monitorPath.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* member, GVariant* params, gpointer self) {
          static_cast<Impl*>(self)->onMonitorSignal(member, params);
        },
        this, nullptr);
    return true;
  }

  void closeMonitor() {
    if (monitorPath.empty()) return;
    if (monitorSub) g_dbus_connection_signal_unsubscribe(bus, monitorSub);
    monitorSub = 0;
    GVariant* r = g_dbus_connection_call_sync(bus, kPortalName, monitorPath.c_str(), kMonitorIface, "Close", nullptr, nullptr,
                                              G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    if (r) g_variant_unref(r);
    monitorPath.clear();
  }

  void onMonitorSignal(const char* member, GVariant* params) {
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(a{sv})"))) return;
    GVariant* d = g_variant_get_child_value(params, 0);
    if (std::string(member) == "UpdateAvailable") {
      std::string running = str(d, "running-commit"), local = str(d, "local-commit"), remote = str(d, "remote-commit");
      std::fprintf(stderr, "updates: UpdateAvailable running %.12s local %.12s remote %.12s\n", running.c_str(), local.c_str(),
                   remote.c_str());
      withModel([&](UpdateModel& md) { md.updateAvailable(running, local, remote); });
      if (classifyUpdate(running, local, remote) == UpdateKind::restartToUse) fetchNewVersion();
    } else if (std::string(member) == "Progress") {
      UpdateProgressInfo p;
      p.op = u32(d, "op");
      p.nOps = u32(d, "n_ops");
      p.progress = u32(d, "progress");
      p.status = u32(d, "status");
      p.error = str(d, "error");
      p.errorMessage = str(d, "error_message");
      if (p.status != kUpdateRunning)
        std::fprintf(stderr, "updates: progress status %u %s %s\n", p.status, p.error.c_str(), p.errorMessage.c_str());
      withModel([&](UpdateModel& md) { md.progress(p); });
      if (p.status != kUpdateRunning) {
        installing = false;
        if (p.status == kUpdateDone) fetchNewVersion();
        if (!autoCheck) closeMonitor();
      }
    }
    g_variant_unref(d);
  }

  void startUpdate(bool fromCheck) {
    if (!portalOK || installing) return;
    if (!openMonitor()) return;
    withModel([&](UpdateModel& md) { fromCheck ? md.checkStarted() : md.updateStarted(); });
    installing = true;
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    GError* e = nullptr;
    GVariant* r = g_dbus_connection_call_sync(bus, kPortalName, monitorPath.c_str(), kMonitorIface, "Update",
                                              g_variant_new("(sa{sv})", "", &opts), nullptr, G_DBUS_CALL_FLAGS_NONE, 30000,
                                              nullptr, &e);
    if (!r) {
      auto [name, msg] = errorParts(e);
      std::fprintf(stderr, "updates: Update failed: %s %s\n", name.c_str(), msg.c_str());
      g_clear_error(&e);
      installing = false;
      withModel([&](UpdateModel& md) { md.callFailed(name, msg); });
      return;
    }
    g_variant_unref(r);
  }

  /// `replaynes-linux --version` of the newest installed version (Spawn, LATEST_VERSION, a
  /// sandbox without network; stdout through a pipe).
  void fetchNewVersion() {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) return;
    GUnixFDList* list = g_unix_fd_list_new();
    GError* e = nullptr;
    int h = g_unix_fd_list_append(list, fds[1], &e);
    ::close(fds[1]);
    if (h < 0) {
      g_clear_error(&e);
      g_object_unref(list);
      ::close(fds[0]);
      return;
    }
    GVariantBuilder argv, fdmap, envs, opts;
    g_variant_builder_init(&argv, G_VARIANT_TYPE("aay"));
    g_variant_builder_add(&argv, "@ay", g_variant_new_bytestring("replaynes-linux"));
    g_variant_builder_add(&argv, "@ay", g_variant_new_bytestring("--version"));
    g_variant_builder_init(&fdmap, G_VARIANT_TYPE("a{uh}"));
    g_variant_builder_add(&fdmap, "{uh}", guint32(1), gint32(h));
    g_variant_builder_init(&envs, G_VARIANT_TYPE("a{ss}"));
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    GVariant* r = g_dbus_connection_call_with_unix_fd_list_sync(
        bus, kPortalName, kPortalPath, kPortalIface, "Spawn",
        g_variant_new("(@ayaaya{uh}a{ss}ua{sv})", g_variant_new_bytestring("/"), &argv, &fdmap, &envs,
                      kSpawnLatestVersion | kSpawnSandbox | kSpawnNoNetwork, &opts),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 30000, list, nullptr, nullptr, &e);
    g_object_unref(list);
    if (!r) {
      auto [name, msg] = errorParts(e);
      std::fprintf(stderr, "updates: Spawn --version failed: %s\n", msg.c_str());
      g_clear_error(&e);
      ::close(fds[0]);
      return;
    }
    g_variant_unref(r);
    std::string out;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    char buf[256];
    while (out.size() < 4096) {
      int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count());
      if (left <= 0) break;
      pollfd p{fds[0], POLLIN, 0};
      if (::poll(&p, 1, left) <= 0) break;
      ssize_t n = ::read(fds[0], buf, sizeof buf);
      if (n <= 0) break;
      out.append(buf, size_t(n));
    }
    ::close(fds[0]);
    std::string v = parseVersionOutput(out);
    std::fprintf(stderr, "updates: installed version %s\n", v.empty() ? "?" : v.c_str());
    if (!v.empty()) withModel([&](UpdateModel& md) { md.setNewVersion(v); });
  }

  void restart(const std::vector<std::string>& args, std::promise<int>* done) {
    if (!portalOK) {
      done->set_value(-1);
      return;
    }
    closeMonitor();
    exitPromise = done;
    exitSub = g_dbus_connection_signal_subscribe(
        bus, nullptr, kPortalIface, "SpawnExited", kPortalPath, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer self) {
          auto* me = static_cast<Impl*>(self);
          guint32 pid = 0, status = 0;
          if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(uu)"))) return;
          g_variant_get(params, "(uu)", &pid, &status);
          if (pid != me->childPid || !me->exitPromise) return;
          int code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
          me->exitPromise->set_value(code);
          me->exitPromise = nullptr;
        },
        this, nullptr);
    GUnixFDList* list = g_unix_fd_list_new();
    GVariantBuilder argv, fdmap, envs, opts;
    g_variant_builder_init(&argv, G_VARIANT_TYPE("aay"));
    g_variant_builder_add(&argv, "@ay", g_variant_new_bytestring("replaynes-linux"));
    for (const std::string& a : args) g_variant_builder_add(&argv, "@ay", g_variant_new_bytestring(a.c_str()));
    g_variant_builder_init(&fdmap, G_VARIANT_TYPE("a{uh}"));
    for (int fd = 0; fd <= 2; ++fd) {  // the same stdin / stdout / stderr (logs go where ours went)
      if (::fcntl(fd, F_GETFD) == -1) continue;
      int h = g_unix_fd_list_append(list, fd, nullptr);
      if (h >= 0) g_variant_builder_add(&fdmap, "{uh}", guint32(fd), gint32(h));
    }
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) env.emplace_back(*e);
    g_variant_builder_init(&envs, G_VARIANT_TYPE("a{ss}"));
    for (const auto& [k, v] : restartEnvironment(env)) g_variant_builder_add(&envs, "{ss}", k.c_str(), v.c_str());
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    char cwd[4096];
    const char* dir = ::getcwd(cwd, sizeof cwd) ? cwd : "/";
    GError* e = nullptr;
    GVariant* r = g_dbus_connection_call_with_unix_fd_list_sync(
        bus, kPortalName, kPortalPath, kPortalIface, "Spawn",
        g_variant_new("(@ayaaya{uh}a{ss}ua{sv})", g_variant_new_bytestring(dir), &argv, &fdmap, &envs,
                      kSpawnLatestVersion | kSpawnWatchBus, &opts),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 30000, list, nullptr, nullptr, &e);
    g_object_unref(list);
    if (!r) {
      auto [name, msg] = errorParts(e);
      std::fprintf(stderr, "updates: restart (Spawn) failed: %s %s\n", name.c_str(), msg.c_str());
      g_clear_error(&e);
      exitPromise = nullptr;
      done->set_value(-1);
      return;
    }
    g_variant_get(r, "(u)", &childPid);
    g_variant_unref(r);
    std::fprintf(stderr, "updates: restarted as pid %u (newest installed version)\n", childPid);
  }
};

FlatpakUpdateService::FlatpakUpdateService() : impl_(std::make_unique<Impl>()) {
  impl_->ctx = g_main_context_new();
  impl_->loop = g_main_loop_new(impl_->ctx, FALSE);
}

FlatpakUpdateService::~FlatpakUpdateService() {
  if (impl_->thread.joinable()) {
    Impl* i = impl_.get();
    i->post([i] { g_main_loop_quit(i->loop); });
    i->thread.join();
  }
  g_main_loop_unref(impl_->loop);
  g_main_context_unref(impl_->ctx);
}

void FlatpakUpdateService::start(bool autoCheck) {
  if (impl_->thread.joinable()) return;
  impl_->thread = std::thread([i = impl_.get(), autoCheck] { i->run(autoCheck); });
}

void FlatpakUpdateService::setAutoCheck(bool on) {
  Impl* i = impl_.get();
  if (!i->thread.joinable()) return;
  i->post([i, on] {
    i->autoCheck = on;
    if (on) i->openMonitor();
    else if (!i->installing) i->closeMonitor();
  });
}

void FlatpakUpdateService::checkNow() {
  Impl* i = impl_.get();
  if (i->thread.joinable()) i->post([i] { i->startUpdate(true); });
}

void FlatpakUpdateService::update() {
  Impl* i = impl_.get();
  if (i->thread.joinable()) i->post([i] { i->startUpdate(false); });
}

void FlatpakUpdateService::dismiss() {
  impl_->withModel([](UpdateModel& md) { md.dismiss(); });
}

UpdateModel FlatpakUpdateService::snapshot() const {
  std::lock_guard<std::mutex> lk(impl_->m);
  return impl_->model;
}

int FlatpakUpdateService::restartLatestAndWait(const std::vector<std::string>& args) {
  Impl* i = impl_.get();
  if (!i->thread.joinable()) return -1;
  std::promise<int> done;
  std::future<int> f = done.get_future();
  i->post([i, args, &done] { i->restart(args, &done); });
  return f.get();
}

#else  // !RNL_HAVE_GIO

struct FlatpakUpdateService::Impl {
  UpdateModel model;
};
FlatpakUpdateService::FlatpakUpdateService() : impl_(std::make_unique<Impl>()) {}
FlatpakUpdateService::~FlatpakUpdateService() = default;
void FlatpakUpdateService::start(bool) { impl_->model.setUnsupported(UpdateUnavailable::notFlatpak); }
void FlatpakUpdateService::setAutoCheck(bool) {}
void FlatpakUpdateService::checkNow() {}
void FlatpakUpdateService::update() {}
void FlatpakUpdateService::dismiss() { impl_->model.dismiss(); }
UpdateModel FlatpakUpdateService::snapshot() const { return impl_->model; }
int FlatpakUpdateService::restartLatestAndWait(const std::vector<std::string>&) { return -1; }

#endif

}  // namespace rnl
