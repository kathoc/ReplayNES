// In-app updates through the XDG Flatpak portal (org.freedesktop.portal.Flatpak on the session
// bus, GDBus): an update monitor (UpdateAvailable / Progress), Update(), and restarting into the
// newest installed version with Spawn(LATEST_VERSION). All D-Bus work runs on the service's own
// thread (a GMainLoop); the frame loop only reads snapshots (UpdateModel, update_model.h) and posts
// requests, so nothing here blocks rendering.
// Without GIO (builds outside the Flatpak SDK) or outside a Flatpak sandbox the service reports
// UpdateUnavailable::notFlatpak and does nothing.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "update_service.h"

namespace rnl {

class FlatpakUpdateService final : public UpdateService {
 public:
  FlatpakUpdateService();
  ~FlatpakUpdateService() override;
  FlatpakUpdateService(const FlatpakUpdateService&) = delete;
  FlatpakUpdateService& operator=(const FlatpakUpdateService&) = delete;

  /// Connects to the portal (on the service thread). autoCheck: keep an update monitor open (the
  /// portal reports updates of the repository while the app runs).
  void start(bool autoCheck) override;
  void setAutoCheck(bool on) override;
  /// Check now: asks the portal to update (it installs an update if there is one; "up to date"
  /// otherwise).
  void checkNow() override;
  void update() override;
  void dismiss() override;
  UpdateModel snapshot() const override;
  /// Starts the newest installed version through the portal and waits until it exits, so a
  /// launcher (Steam) still sees the game running.
  int restartLatestAndWait(const std::vector<std::string>& args) override;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

/// Whether this process runs inside a Flatpak sandbox (/.flatpak-info).
bool inFlatpakSandbox();

}  // namespace rnl
