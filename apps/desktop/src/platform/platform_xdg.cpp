// Linux (and the macOS test build): XDG folders, the freedesktop.org Trash, gamescope detection.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "paths.h"
#include "platform/platform.h"
#include "platform/trash_xdg.h"

namespace fs = std::filesystem;

namespace rnl {

namespace {
std::string env(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}
/// XDG_DOCUMENTS_DIR from the environment or user-dirs.dirs (XDG_DOCUMENTS_DIR="$HOME/Docs").
std::string xdgDocumentsDir(const std::string& home) {
  std::string v = env("XDG_DOCUMENTS_DIR");
  if (!v.empty()) return Paths::stripSlash(v);
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
    if (!val.empty() && val[0] == '/') return Paths::stripSlash(val);
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

std::string Paths::display(const std::string& path) {
  std::string home = env("HOME");
  if (!home.empty() && path.compare(0, home.size(), home) == 0 && (path.size() == home.size() || path[home.size()] == '/'))
    return "~" + path.substr(home.size());
  return path;
}

bool trashItem(const std::string& path, std::string* error) {
  return moveToTrash(path, homeTrashDir(), double(std::time(nullptr)), nullptr, error);
}

bool gamingMode() { return std::getenv("GAMESCOPE_WAYLAND_DISPLAY") || std::getenv("SteamGamepadUI"); }

}  // namespace rnl
