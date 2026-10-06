// SPDX-License-Identifier: GPL-2.0-or-later
#include "paths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "replaynes/frontend.h"

namespace fs = std::filesystem;

namespace rnl {

namespace {
std::string env(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}
std::string stripSlash(std::string s) {
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}
/// XDG_DOCUMENTS_DIR from the environment or user-dirs.dirs (XDG_DOCUMENTS_DIR="$HOME/Docs").
std::string xdgDocumentsDir(const std::string& home) {
  std::string v = env("XDG_DOCUMENTS_DIR");
  if (!v.empty()) return stripSlash(v);
  std::string config = env("XDG_CONFIG_HOME");
  if (config.empty()) config = home + "/.config";
  std::ifstream f(config + "/user-dirs.dirs");
  std::string line;
  while (std::getline(f, line)) {
    const std::string key = "XDG_DOCUMENTS_DIR=";
    if (line.compare(0, key.size(), key) != 0) continue;
    std::string val = line.substr(key.size());
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
    if (val.compare(0, 5, "$HOME") == 0) val = home + val.substr(5);
    if (!val.empty() && val[0] == '/') return stripSlash(val);
  }
  return {};
}
}  // namespace

Paths Paths::standard() {
  Paths p;
  std::string home = env("HOME");
  std::string docs = home + "/Documents";
  std::error_code ec;
  if (home.empty() || !fs::is_directory(docs, ec)) {
    std::string x = xdgDocumentsDir(home);
    if (!x.empty()) docs = x;
  }
  p.setLibraryRoot(docs + "/ReplayNES");
  std::string data = env("XDG_DATA_HOME");
  if (data.empty()) data = home + "/.local/share";
  std::string config = env("XDG_CONFIG_HOME");
  if (config.empty()) config = home + "/.config";
  p.sessionRoot = data + "/ReplayNES/Session";
  p.configDir = config + "/ReplayNES";
  return p;
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

std::string Paths::display(const std::string& path) {
  std::string home = env("HOME");
  if (!home.empty() && path.compare(0, home.size(), home) == 0 && (path.size() == home.size() || path[home.size()] == '/'))
    return "~" + path.substr(home.size());
  return path;
}

}  // namespace rnl
