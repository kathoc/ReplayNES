// XDG locations (docs/plans/2026-10-07-steam-deck-plan.md, "Paths"):
//   library root     ~/Documents/ReplayNES (XDG documents dir when ~/Documents is absent)
//     ROM/           the user's ROMs        Projects/  projects started from the library
//   session          $XDG_DATA_HOME/ReplayNES/Session   (current.nesrec = temporary project,
//                    resume.json, .lock: the shared core's session folder layout)
//   settings         $XDG_CONFIG_HOME/ReplayNES  (linux-settings.ini, bindings.json)
// Inside the Flatpak, XDG_DATA_HOME / XDG_CONFIG_HOME point into ~/.var/app/<app id>/.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace rnl {

struct Paths {
  std::string libraryRoot;
  std::string romDir;
  std::string projectsDir;
  std::string sessionRoot;
  std::string configDir;

  static Paths standard();
  /// The library root moved (tests, --library-root).
  void setLibraryRoot(const std::string& root);
  /// Creates the session and config folders (the library folders: LibraryModel::ensure).
  void ensure() const;
  std::string tempProject() const;
  std::string resumeFile() const;
  std::string lockFile() const;
  std::string settingsFile() const { return configDir + "/linux-settings.ini"; }
  std::string bindingsFile() const { return configDir + "/bindings.json"; }
  /// "~/..." for display.
  static std::string display(const std::string& path);
};

}  // namespace rnl
