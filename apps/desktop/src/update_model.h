// In-app updates of the Flatpak (Linux / Steam Deck) through the XDG Flatpak portal
// (org.freedesktop.portal.Flatpak): the state the UI shows, driven by the portal's events.
// Pure logic (no D-Bus): UpdateService (update_service.h) feeds it from its D-Bus thread and the
// UI reads copies of it; the tests drive it directly.
//
// The portal's update monitor checks the app's origin remote periodically (flatpak-portal polls
// every 30 minutes) and reports UpdateAvailable with three commits: running (this process),
// local (installed) and remote. Update() checks and installs in one step and reports Progress
// (status running / empty = nothing to update / done / error). Updates only work when the app
// was installed from a repository (the .flatpakref, or a bundle built with --repo-url).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rnl {

enum class UpdatePhase {
  unsupported,  // not a Flatpak / no portal: updates are unavailable (see UpdateModel::reason)
  idle,         // nothing to show (monitoring, or waiting for "Check now")
  checking,     // Update() called (Check now / Update), no progress yet
  available,    // a newer commit is in the repository
  updating,     // downloading / installing
  installed,    // a newer version is installed: restart to use it
  upToDate,     // Check now found nothing to update
  failed,       // the last check / update failed (see error, reason)
};

enum class UpdateUnavailable {
  none,
  notFlatpak,    // not running inside a Flatpak sandbox (developer build)
  noPortal,      // no org.freedesktop.portal.Flatpak on the session bus
  noRepository,  // installed without a repository (bundle without --repo-url): nothing to update from
};

/// Why an update failed, for the message shown.
enum class UpdateError {
  none,
  other,
  denied,          // the user (or the permission store) did not allow the app to update itself
  newPermissions,  // the new version needs more permissions: update with `flatpak update`
  noDialog,        // no portal UI to ask for the update permission (allow it once from a terminal)
  noRepository,    // installed without a repository
  network,         // the repository could not be reached
};

/// The portal's Progress status values.
enum : uint32_t { kUpdateRunning = 0, kUpdateEmpty = 1, kUpdateDone = 2, kUpdateError = 3 };

struct UpdateProgressInfo {
  uint32_t op = 0, nOps = 0, progress = 0;  // progress: 0-100 of operation `op` (of nOps)
  uint32_t status = kUpdateRunning;
  std::string error;         // D-Bus error name (status error)
  std::string errorMessage;  // its message
};

/// What an UpdateAvailable report means.
enum class UpdateKind {
  none,           // running == local == remote (or unknown commits)
  download,       // the repository has a newer commit than the installed one
  restartToUse,   // a newer version is installed already (e.g. `flatpak update`): restart to use it
};
UpdateKind classifyUpdate(const std::string& running, const std::string& local, const std::string& remote);

/// Overall percentage of an update with several operations (runtime + app ...), 0-100.
int overallPercent(uint32_t op, uint32_t nOps, uint32_t progress);

/// The kind of a failure from the D-Bus error name and message of the portal / flatpak.
UpdateError classifyUpdateError(const std::string& name, const std::string& message);

class UpdateModel {
 public:
  // Inputs.
  void setUnsupported(UpdateUnavailable why);
  void portalReady();
  void updateAvailable(const std::string& running, const std::string& local, const std::string& remote);
  void checkStarted();   // Check now (Update() that may find nothing)
  void updateStarted();  // Update (after UpdateAvailable)
  void progress(const UpdateProgressInfo& p);
  void callFailed(const std::string& name, const std::string& message);  // Update() itself failed
  void setNewVersion(const std::string& v) { newVersion = v; }
  /// "Later": hide the notice until something new is reported (another commit, a finished update).
  void dismiss();

  // State.
  UpdatePhase phase = UpdatePhase::idle;
  UpdateUnavailable reason = UpdateUnavailable::none;
  UpdateError error = UpdateError::none;
  std::string errorMessage;   // as reported (shown under the generic text)
  int percent = 0;            // updating
  bool userInitiated = false; // the current / last operation came from Check now
  bool noticeHidden = false;  // "Later" on the current notice
  std::string newVersion;     // version of the installed update ("" until known)
  std::string remoteCommit;   // last reported remote commit
  uint64_t serial = 0;        // changes with every state change (UI: something new to show)

  /// The library / hub notice shows (an update to download or to restart into, progress, errors
  /// of an update the user started).
  bool noticeVisible() const;
  bool busy() const { return phase == UpdatePhase::checking || phase == UpdatePhase::updating; }

 private:
  void touch() { ++serial; }
};

/// Arguments for restarting into the new version: argv without the program name and without
/// one-shot options (--script CMDS, --perf-seconds N, ...) that must not run again.
std::vector<std::string> restartArguments(const std::vector<std::string>& argv);

/// Environment variables passed to the restarted instance (from "KEY=VALUE" entries): language,
/// SDL / Steam / gamescope settings and REPLAYNES_*; never the sandbox's own paths or sockets
/// (PATH, DISPLAY, XDG_*_HOME ... are set up again by flatpak for the new instance).
std::vector<std::pair<std::string, std::string>> restartEnvironment(const std::vector<std::string>& environment);

/// The version printed by `replaynes-linux --version` ("ReplayNES 0.3.1" -> "0.3.1"), "" if none.
std::string parseVersionOutput(const std::string& text);

}  // namespace rnl
