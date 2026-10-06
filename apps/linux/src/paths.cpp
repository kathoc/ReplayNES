// SPDX-License-Identifier: GPL-2.0-or-later
#include "paths.h"

#include <SDL3/SDL.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>

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
bool isNes(const fs::path& p) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return e == ".nes";
}
}  // namespace

Paths Paths::standard() {
  Paths p;
  std::string home = env("HOME");
  std::string docs = home + "/Documents";
  std::error_code ec;
  if (home.empty() || !fs::is_directory(docs, ec)) {
    if (const char* d = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)) docs = stripSlash(d);
  }
  p.romDir = docs + "/ReplayNES/ROM";
  p.projectsDir = docs + "/ReplayNES/Projects";
  std::string data = env("XDG_DATA_HOME");
  if (data.empty()) data = home + "/.local/share";
  std::string config = env("XDG_CONFIG_HOME");
  if (config.empty()) config = home + "/.config";
  p.sessionRoot = data + "/ReplayNES/Session";
  p.configDir = config + "/ReplayNES";
  return p;
}

void Paths::ensure() const {
  std::error_code ec;
  for (const std::string& d : {romDir, projectsDir, sessionRoot, configDir}) fs::create_directories(d, ec);
}

std::vector<std::string> listRoms(const std::string& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && isNes(it->path())) out.push_back(it->path().string());
    else if (it->is_directory(ec)) {
      std::error_code ec2;
      for (fs::directory_iterator s(it->path(), ec2), e2; !ec2 && s != e2; s.increment(ec2))
        if (s->is_regular_file(ec2) && isNes(s->path())) out.push_back(s->path().string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool lockSessionRoot(const Paths& p) {
  int fd = ::open(p.lockFile().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return true;  // cannot lock (read-only?): do not block the app
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); return false; }
  return true;  // fd stays open (and locked) for the life of the process
}

}  // namespace rnl
