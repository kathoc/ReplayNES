// In-app updates as the UI sees them (state: UpdateModel, update_model.h). One implementation per
// platform that has them:
//   Linux    FlatpakUpdateService (apps/linux/src/update_service_flatpak.h): the XDG Flatpak portal
//   Windows  none yet (the UI gets no service and shows only the version)
// Implementations do their work on their own thread; the frame loop only reads snapshots and posts
// requests, so nothing here blocks rendering.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

#include "update_model.h"

namespace rnl {

class UpdateService {
 public:
  virtual ~UpdateService() = default;
  /// Connects (on the service thread). autoCheck: keep watching for updates while the app runs.
  virtual void start(bool autoCheck) = 0;
  virtual void setAutoCheck(bool on) = 0;
  /// Check now: installs an update if there is one ("up to date" otherwise).
  virtual void checkNow() = 0;
  /// Update after an "available" report.
  virtual void update() = 0;
  /// Later: hides the notice until something new is reported.
  virtual void dismiss() = 0;
  virtual UpdateModel snapshot() const = 0;
  /// After the app has shut down (window, audio, session lock released): starts the newest
  /// installed version with these arguments (restartArguments()) and waits until it exits.
  /// Returns its exit code, or -1 when it could not be started.
  virtual int restartLatestAndWait(const std::vector<std::string>& args) = 0;
};

}  // namespace rnl
