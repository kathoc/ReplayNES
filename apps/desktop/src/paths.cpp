// SPDX-License-Identifier: GPL-2.0-or-later
// Paths::standard() / Paths::display() are per platform: platform/platform_xdg.cpp,
// platform/platform_windows.cpp.
#include "paths.h"

#include <filesystem>
#include <system_error>

#include "replaynes/frontend.h"

namespace fs = std::filesystem;

namespace rnl {

std::string Paths::stripSlash(std::string s) {
  while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
  return s;
}

void Paths::setLibraryRoot(const std::string& root) {
  libraryRoot = stripSlash(root);
  romDir = libraryRoot + "/" + RNF_LIBRARY_ROM_DIR;
  projectsDir = libraryRoot + "/" + RNF_LIBRARY_PROJECTS_DIR;
}

void Paths::ensure() const {
  std::error_code ec;
  for (const std::string& d : {sessionRoot, configDir}) fs::create_directories(d, ec);
}

std::string Paths::tempProject() const { return sessionRoot + "/" + RNF_SESSION_TEMP_PROJECT; }
std::string Paths::resumeFile() const { return sessionRoot + "/" + RNF_SESSION_RESUME_FILE; }
std::string Paths::lockFile() const { return sessionRoot + "/" + RNF_SESSION_LOCK_FILE; }

}  // namespace rnl
