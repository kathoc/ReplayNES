// XDG locations (docs/plans/2026-10-07-steam-deck-plan.md, "Paths"):
//   ROM library      ~/Documents/ReplayNES/ROM   (XDG documents dir when ~/Documents is absent)
//   projects         ~/Documents/ReplayNES/Projects
//   session          $XDG_DATA_HOME/ReplayNES/Session   (current.nesrec = temporary project, .lock)
//   settings         $XDG_CONFIG_HOME/ReplayNES
// Inside the Flatpak, XDG_DATA_HOME / XDG_CONFIG_HOME point into ~/.var/app/<app id>/.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

namespace rnl {

struct Paths {
  std::string romDir;
  std::string projectsDir;
  std::string sessionRoot;
  std::string configDir;

  static Paths standard();
  /// Creates the directories (errors are ignored here; opening files reports them).
  void ensure() const;
  std::string tempProject() const { return sessionRoot + "/current.nesrec"; }
  std::string lockFile() const { return sessionRoot + "/.lock"; }
  std::string settingsFile() const { return configDir + "/linux-settings.ini"; }
};

/// *.nes files (any case) in dir and its direct subdirectories, sorted by name.
std::vector<std::string> listRoms(const std::string& dir);

/// flock()s the session folder for this instance. Returns false if another instance holds it.
bool lockSessionRoot(const Paths& p);

}  // namespace rnl
