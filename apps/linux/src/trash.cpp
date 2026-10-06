// SPDX-License-Identifier: GPL-2.0-or-later
#include "trash.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace rnl {

namespace {
std::string env(const char* n) {
  const char* v = std::getenv(n);
  return v ? std::string(v) : std::string();
}
bool writeAll(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::write(fd, s.data() + off, s.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += size_t(n);
  }
  return true;
}
}  // namespace

std::string homeTrashDir() {
  std::string home = env("HOME");
  std::string data = env("HOST_XDG_DATA_HOME");
  if (data.empty() && !env("FLATPAK_ID").empty()) data = home + "/.local/share";
  if (data.empty()) data = env("XDG_DATA_HOME");
  if (data.empty()) data = home + "/.local/share";
  return data + "/Trash";
}

std::string trashEscapePath(const std::string& p) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : p) {
    bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.' || c == '~' || c == '/';
    if (keep) {
      out += char(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string trashInfoContents(const std::string& absolutePath, double date) {
  std::time_t t = std::time_t(std::floor(date));
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
  return "[Trash Info]\nPath=" + trashEscapePath(absolutePath) + "\nDeletionDate=" + buf + "\n";
}

std::string trashCandidateName(const std::string& name, int n) {
  if (n <= 1) return name;
  std::string stem = name, ext;
  size_t dot = name.rfind('.');
  if (dot != std::string::npos && dot > 0) {
    stem = name.substr(0, dot);
    ext = name.substr(dot);
  }
  return stem + " " + std::to_string(n) + ext;
}

bool copyTree(const std::string& from, const std::string& to, std::string* error) {
  std::error_code ec;
  if (fs::exists(to, ec)) {
    if (error) *error = to + ": already exists";
    return false;
  }
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
  if (ec) {
    std::error_code ec2;
    fs::remove_all(to, ec2);
    if (error) *error = from + " -> " + to + ": " + ec.message();
    return false;
  }
  return true;
}

bool moveToTrash(const std::string& path, const std::string& trashDir, double date, std::string* trashedAs,
                 std::string* error) {
  std::error_code ec;
  fs::path src = fs::absolute(path, ec);
  if (ec || !fs::exists(fs::symlink_status(src, ec))) {
    if (error) *error = path + ": not found";
    return false;
  }
  src = src.lexically_normal();
  std::string name = src.filename().string();
  if (name.empty()) name = src.parent_path().filename().string();  // trailing separator
  std::string filesDir = trashDir + "/files", infoDir = trashDir + "/info";
  for (const std::string& d : {trashDir, filesDir, infoDir}) {
    if (!fs::is_directory(d, ec)) {
      fs::create_directories(d, ec);
      if (ec) {
        if (error) *error = d + ": " + ec.message();
        return false;
      }
      ::chmod(d.c_str(), 0700);
    }
  }
  std::string info = trashInfoContents(src.string(), date);
  for (int n = 1; n < 10000; ++n) {
    std::string cand = trashCandidateName(name, n);
    std::string infoPath = infoDir + "/" + cand + ".trashinfo";
    std::string dest = filesDir + "/" + cand;
    if (fs::exists(fs::symlink_status(dest, ec))) continue;
    int fd = ::open(infoPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      if (errno == EEXIST) continue;
      if (error) *error = infoPath + ": " + std::strerror(errno);
      return false;
    }
    bool ok = writeAll(fd, info);
    ::close(fd);
    if (!ok) {
      ::unlink(infoPath.c_str());
      if (error) *error = infoPath + ": write failed";
      return false;
    }
    if (::rename(src.c_str(), dest.c_str()) != 0) {
      if (errno != EXDEV) {
        std::string msg = std::strerror(errno);
        ::unlink(infoPath.c_str());
        if (error) *error = src.string() + ": " + msg;
        return false;
      }
      // Another file system: copy, then remove the original.
      std::string err;
      if (!copyTree(src.string(), dest, &err)) {
        ::unlink(infoPath.c_str());
        if (error) *error = err;
        return false;
      }
      fs::remove_all(src, ec);
      if (ec) {
        // The original is still there: undo, so nothing is trashed twice.
        std::error_code ec2;
        fs::remove_all(dest, ec2);
        ::unlink(infoPath.c_str());
        if (error) *error = src.string() + ": " + ec.message();
        return false;
      }
    }
    if (trashedAs) *trashedAs = dest;
    return true;
  }
  if (error) *error = "no free name in " + filesDir;
  return false;
}

}  // namespace rnl
