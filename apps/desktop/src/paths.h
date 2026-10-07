// Where the desktop frontend keeps its files (Paths::standard(), per platform):
//
// Linux (XDG; docs/plans/2026-10-07-steam-deck-plan.md, "Paths"; platform/platform_xdg.cpp):
//   library root     ~/Documents/ReplayNES (XDG documents dir when ~/Documents is absent)
//     ROM/           the user's ROMs        Projects/  projects started from the library
//   session          $XDG_DATA_HOME/ReplayNES/Session   (current.nesrec = temporary project,
//                    resume.json, .lock: the shared core's session folder layout)
//   settings         $XDG_CONFIG_HOME/ReplayNES  (linux-settings.ini, bindings.json)
//   Inside the Flatpak, XDG_DATA_HOME / XDG_CONFIG_HOME point into ~/.var/app/<app id>/.
//
// Windows (Known Folders; platform/platform_windows.cpp):
//   library root     Documents\ReplayNES  (FOLDERID_Documents; ROM\, Projects\)
//   session          %LOCALAPPDATA%\ReplayNES\Session
//   settings         %APPDATA%\ReplayNES  (settings.ini, bindings.json)
//
// Paths are UTF-8 (Windows: the executables declare the UTF-8 code page in their manifest, so
// narrow file APIs and std::filesystem take UTF-8). Joined with '/', which Windows accepts.
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
#ifdef _WIN32
  std::string settingsFile() const { return configDir + "/settings.ini"; }
#else
  std::string settingsFile() const { return configDir + "/linux-settings.ini"; }
#endif
  std::string bindingsFile() const { return configDir + "/bindings.json"; }
  /// For display: "~/..." on Linux; native separators on Windows.
  static std::string display(const std::string& path);
  /// Without trailing separators.
  static std::string stripSlash(std::string s);
};

}  // namespace rnl
