// ROM library (UI-free part):
//   <root>/ROM/       the user's .nes files (top level and one level of sub-folders)
//   <root>/Projects/  projects started from the library, "<ROM name> <yyyy-MM-dd HHmm>.nesrec"
// Projects are matched to ROMs by the ROM SHA-256 stored in their manifest.json, not by name.
// Also the backup name used by "Reset Project".
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <sys/stat.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "common.hpp"
#include "util/Json.h"

using namespace rnf;
namespace stdfs = std::filesystem;

namespace {

// Date.distantPast (0001-01-01) in seconds since 1970.
constexpr double kDistantPast = -62135769600.0;

struct Entry {
  bool ok = false;
  bool isDir = false;
  bool isRegular = false;
  int64_t size = 0;
  double modified = kDistantPast;
};

// Properties of the item itself (symbolic links are not followed, like URL resource values).
Entry inspect(const std::string& path) {
  Entry e;
#ifdef _WIN32
  // Wide API (UTF-8 paths); the reparse point itself, like lstat. The modification time comes
  // straight from the FILETIME so repeated calls are bit-identical (it keys the ROM hash cache).
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (!GetFileAttributesExW(stdfs::u8path(path).c_str(), GetFileExInfoStandard, &a)) return e;
  e.ok = true;
  bool reparse = (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  e.isDir = !reparse && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  e.isRegular = !reparse && !e.isDir;
  e.size = int64_t((uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow);
  // 100 ns ticks since 1601-01-01 -> seconds since 1970-01-01.
  int64_t ticks = int64_t((uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime);
  e.modified = double(ticks - 116444736000000000LL) / 1e7;
#else
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) return e;
  e.ok = true;
  e.isDir = S_ISDIR(st.st_mode);
  e.isRegular = S_ISREG(st.st_mode);
  e.size = int64_t(st.st_size);
#ifdef __APPLE__
  e.modified = double(st.st_mtimespec.tv_sec) + double(st.st_mtimespec.tv_nsec) / 1e9;
#else
  e.modified = double(st.st_mtim.tv_sec) + double(st.st_mtim.tv_nsec) / 1e9;
#endif
#endif
  return e;
}

bool isHidden(const std::string& dir, const std::string& name) {
  if (name.empty() || name[0] == '.') return true;
#ifdef __APPLE__
  struct stat st;
  std::string p = joinPath(dir, name);
  if (::lstat(p.c_str(), &st) == 0 && (st.st_flags & UF_HIDDEN)) return true;
#elif defined(_WIN32)
  DWORD attr = GetFileAttributesW(stdfs::u8path(joinPath(dir, name)).c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_HIDDEN)) return true;
#else
  (void)dir;
#endif
  return false;
}

// Non-hidden entries of a directory. False (with errno text) if it cannot be read.
bool listDir(const std::string& dir, std::vector<std::string>& names, std::string& error) {
  names.clear();
#ifdef _WIN32
  std::error_code ec;
  stdfs::directory_iterator it(stdfs::u8path(dir), ec), end;
  if (ec) { error = ec.message(); return false; }
  for (; it != end; it.increment(ec)) {
    if (ec) { error = ec.message(); return false; }
    std::string n = it->path().filename().u8string();
    if (!isHidden(dir, n)) names.push_back(n);
  }
#else
  DIR* d = ::opendir(dir.c_str());
  if (!d) {
    error = std::strerror(errno);
    return false;
  }
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    if (!isHidden(dir, n)) names.push_back(n);
  }
  ::closedir(d);
#endif
  return true;
}

std::string join(const std::string& dir, const std::string& name) { return joinPath(dir, name); }

std::string stripTrailing(std::string s) { return stripTrailingSeps(std::move(s)); }

std::string lastComponent(const std::string& p) {
  std::string s = stripTrailing(p);
  size_t slash = lastPathSep(s);
  return slash == std::string::npos ? s : s.substr(slash + 1);
}

// URL.pathExtension / deletingPathExtension on a file name (a leading dot is not an extension).
std::string extensionOf(const std::string& name) {
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return "";
  return name.substr(dot + 1);
}
std::string stem(const std::string& name) {
  std::string ext = extensionOf(name);
  return ext.empty() ? name : name.substr(0, name.size() - ext.size() - 1);
}

// Case-insensitive natural order ("game9" < "game10"), the default when no compare is given.
int naturalCompare(const char* a, const char* b) {
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
    if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
      const char *ea = a, *eb = b;
      while (*ea == '0') ++ea;
      while (*eb == '0') ++eb;
      const char *da = ea, *db = eb;
      while (*da >= '0' && *da <= '9') ++da;
      while (*db >= '0' && *db <= '9') ++db;
      if (da - ea != db - eb) return da - ea < db - eb ? -1 : 1;
      for (; ea < da; ++ea, ++eb) if (*ea != *eb) return *ea < *eb ? -1 : 1;
      a = da;
      b = db;
      continue;
    }
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
    if (ca != cb) return ca < cb ? -1 : 1;
    ++a;
    ++b;
  }
  if (*a) return 1;
  if (*b) return -1;
  return 0;
}

// ------------------------------------------------------------------ sanitize
// Next code point of UTF-8 text (invalid bytes become U+FFFD, one byte each).
uint32_t decodeUTF8(const std::string& s, size_t& i) {
  unsigned char c = (unsigned char)s[i];
  int n;
  uint32_t cp;
  if (c < 0x80) { ++i; return c; }
  if ((c >> 5) == 6) { n = 1; cp = c & 0x1F; }
  else if ((c >> 4) == 14) { n = 2; cp = c & 0x0F; }
  else if ((c >> 3) == 30) { n = 3; cp = c & 0x07; }
  else { ++i; return 0xFFFD; }
  for (int k = 1; k <= n; ++k) {
    size_t j = i + size_t(k);
    if (j >= s.size() || ((unsigned char)s[j] >> 6) != 2) { ++i; return 0xFFFD; }
    cp = (cp << 6) | ((unsigned char)s[j] & 0x3F);
  }
  i += size_t(n) + 1;
  return cp;
}

// CharacterSet.controlCharacters: general categories Cc and Cf.
bool isControl(uint32_t c) {
  if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) return true;
  static const uint32_t cf[][2] = {
      {0x00AD, 0x00AD}, {0x0600, 0x0605}, {0x061C, 0x061C}, {0x06DD, 0x06DD}, {0x070F, 0x070F},
      {0x0890, 0x0891}, {0x08E2, 0x08E2}, {0x180E, 0x180E}, {0x200B, 0x200F}, {0x202A, 0x202E},
      {0x2060, 0x2064}, {0x2066, 0x206F}, {0xFEFF, 0xFEFF}, {0xFFF9, 0xFFFB}, {0x110BD, 0x110BD},
      {0x110CD, 0x110CD}, {0x13430, 0x1343F}, {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0001, 0xE0001},
      {0xE0020, 0xE007F},
  };
  for (auto& r : cf) if (c >= r[0] && c <= r[1]) return true;
  return false;
}

// CharacterSet.whitespaces: general category Zs and tab.
bool isWhitespace(uint32_t c) {
  return c == 0x09 || c == 0x20 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F ||
         c == 0x205F || c == 0x3000;
}

std::string sanitize(const std::string& name) {
  std::vector<std::pair<uint32_t, std::string>> chars;  // code point + its bytes
  for (size_t i = 0; i < name.size();) {
    size_t start = i;
    uint32_t cp = decodeUTF8(name, i);
    if (cp == '/' || cp == ':' || cp == '\\' || isControl(cp)) chars.push_back({'_', "_"});
    else chars.push_back({cp, name.substr(start, i - start)});
  }
  size_t lo = 0, hi = chars.size();
  while (lo < hi && isWhitespace(chars[lo].first)) ++lo;
  while (hi > lo && isWhitespace(chars[hi - 1].first)) --hi;
  std::string out;
  for (size_t i = lo; i < hi; ++i) out += chars[i].second;
  if (!out.empty() && out[0] == '.') out = "_" + out.substr(1);
  return out.empty() ? "ROM" : out;
}

std::string localTime(double date, const char* fmt) {
  time_t t = time_t(std::floor(date));
  struct tm tmv;
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char buf[64];
  std::strftime(buf, sizeof buf, fmt, &tmv);
  return buf;
}

bool exists(const std::string& p) {
#ifdef _WIN32
  std::error_code ec;
  return stdfs::exists(stdfs::u8path(p), ec);
#else
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
#endif
}

}  // namespace

struct rnf_rom_list {
  struct Item {
    std::string path, name, relative;
    int64_t size;
    double modified;
  };
  std::vector<Item> items;
};

struct rnf_project_list {
  struct Item {
    std::string path, name, sha, romName;
    double modified;
  };
  std::vector<Item> items;
};

struct rnf_rom_hash_cache {
  std::mutex lock;
  std::map<std::tuple<std::string, int64_t, double>, std::string> cache;
  std::map<std::tuple<std::string, int64_t, double>, std::pair<rnf_game_match, rnf_game_info>> games;
};

extern "C" {

rn_status rnf_library_ensure(const char* root, char** failed_path) {
  if (failed_path) *failed_path = nullptr;
  if (!root) return fail(RN_ERR_INVALID_ARG, "null root");
  RNF_GUARD_BEGIN
  for (const char* sub : {RNF_LIBRARY_ROM_DIR, RNF_LIBRARY_PROJECTS_DIR}) {
    std::string dir = join(root, sub);
    if (exists(dir)) {
      std::error_code dec;
      if (!stdfs::is_directory(stdfs::u8path(dir), dec)) {
        if (failed_path) *failed_path = dup(dir);
        return fail(RN_ERR_ALREADY_EXISTS, "not a directory: " + dir);
      }
      continue;
    }
    std::error_code ec;
    stdfs::create_directories(stdfs::u8path(dir), ec);
    if (ec) {
      if (failed_path) *failed_path = dup(dir);
      return fail(RN_ERR_IO, ec.message());
    }
  }
  return RN_OK;
  RNF_GUARD_END(RN_ERR_INTERNAL)
}

rn_status rnf_library_scan_roms(const char* dir_c, rnf_name_compare compare, void* ctx, rnf_rom_list** out) {
  if (out) *out = nullptr;
  if (!dir_c || !out) return fail(RN_ERR_INVALID_ARG, "null argument");
  RNF_GUARD_BEGIN
  std::string dir = stripTrailing(dir_c);
  std::vector<std::string> names;
  std::string err;
  if (!listDir(dir, names, err)) return fail(RN_ERR_IO, err);
  auto list = std::make_unique<rnf_rom_list>();
  auto add = [&](const std::string& path, const std::string& name, const std::string& relative) {
    if (lower(extensionOf(name)) != "nes") return;
    Entry e = inspect(path);
    if (!e.ok || !e.isRegular) return;
    list->items.push_back({path, stem(name), relative, e.size, e.modified});
  };
  for (auto& n : names) {
    std::string path = join(dir, n);
    Entry e = inspect(path);
    if (e.ok && e.isDir) {
      // One level of sub-folders; an unreadable sub-folder is skipped, not fatal.
      std::vector<std::string> subs;
      std::string ignored;
      if (!listDir(path, subs, ignored)) continue;
      for (auto& s : subs) add(join(path, s), s, n + "/" + s);
    } else {
      add(path, n, n);
    }
  }
  std::stable_sort(list->items.begin(), list->items.end(), [&](const auto& a, const auto& b) {
    int c = compare ? compare(a.name.c_str(), b.name.c_str(), ctx) : naturalCompare(a.name.c_str(), b.name.c_str());
    return c == 0 ? a.relative < b.relative : c < 0;
  });
  *out = list.release();
  return RN_OK;
  RNF_GUARD_END(RN_ERR_INTERNAL)
}

size_t rnf_rom_list_count(const rnf_rom_list* l) { return l ? l->items.size() : 0; }
int rnf_rom_list_get(const rnf_rom_list* l, size_t i, rnf_rom_entry* out) {
  if (!l || i >= l->items.size() || !out) return 0;
  auto& it = l->items[i];
  out->path = it.path.c_str();
  out->name = it.name.c_str();
  out->relative_path = it.relative.c_str();
  out->size = it.size;
  out->modified = it.modified;
  return 1;
}
void rnf_rom_list_free(rnf_rom_list* l) { delete l; }

rn_status rnf_library_scan_projects(const char* dir_c, rnf_project_list** out) {
  if (out) *out = nullptr;
  if (!dir_c || !out) return fail(RN_ERR_INVALID_ARG, "null argument");
  RNF_GUARD_BEGIN
  std::string dir = stripTrailing(dir_c);
  std::vector<std::string> names;
  std::string err;
  if (!listDir(dir, names, err)) return fail(RN_ERR_IO, err);
  auto list = std::make_unique<rnf_project_list>();
  for (auto& n : names) {
    if (lower(extensionOf(n)) != "nesrec") continue;
    std::string path = join(dir, n);
    char* text = nullptr;
    if (rn_project_manifest_json(path.c_str(), &text) != RN_OK || !text) continue;
    rn::Json m;
    bool ok = rn::Json::parse(text, m);
    rn_string_free(text);
    if (!ok || !m.isObject()) continue;
    const rn::Json& rom = m["rom"];
    if (!rom.isObject() || !rom["sha256"].isString()) continue;
    // The manifest is rewritten on every save: its date is the project's last save.
    Entry me = inspect(join(path, "manifest.json"));
    double date = me.ok ? me.modified : kDistantPast;
    if (!me.ok) {
      Entry de = inspect(path);
      if (de.ok) date = de.modified;
    }
    list->items.push_back({path, stem(n), lower(rom["sha256"].asString()),
                           rom["name"].isString() ? rom["name"].asString() : "", date});
  }
  std::stable_sort(list->items.begin(), list->items.end(),
                   [](const auto& a, const auto& b) { return a.modified > b.modified; });
  *out = list.release();
  return RN_OK;
  RNF_GUARD_END(RN_ERR_INTERNAL)
}

size_t rnf_project_list_count(const rnf_project_list* l) { return l ? l->items.size() : 0; }
int rnf_project_list_get(const rnf_project_list* l, size_t i, rnf_project_entry* out) {
  if (!l || i >= l->items.size() || !out) return 0;
  auto& it = l->items[i];
  out->path = it.path.c_str();
  out->name = it.name.c_str();
  out->rom_sha256 = it.sha.c_str();
  out->rom_name = it.romName.c_str();
  out->modified = it.modified;
  return 1;
}
void rnf_project_list_free(rnf_project_list* l) { delete l; }

char* rnf_library_sanitize(const char* name) {
  RNF_GUARD_BEGIN
  return dup(sanitize(name ? name : ""));
  RNF_GUARD_END(nullptr)
}

char* rnf_library_timestamp(double date) {
  RNF_GUARD_BEGIN
  return dup(localTime(date, "%Y-%m-%d %H%M"));
  RNF_GUARD_END(nullptr)
}

char* rnf_library_new_project_path(const char* projects_dir, const char* rom_name, double date) {
  if (!projects_dir) return nullptr;
  RNF_GUARD_BEGIN
  std::string base = sanitize(rom_name ? rom_name : "") + " " + localTime(date, "%Y-%m-%d %H%M");
  std::string dir = stripTrailing(projects_dir);
  for (int n = 1;; ++n) {
    std::string name = n == 1 ? base : base + " " + std::to_string(n);
    std::string path = join(dir, name + ".nesrec");
    if (!exists(path)) return dup(path);
  }
  RNF_GUARD_END(nullptr)
}

rnf_rom_hash_cache* rnf_rom_hash_cache_new(void) {
  try { return new rnf_rom_hash_cache; } catch (...) { return nullptr; }
}
void rnf_rom_hash_cache_free(rnf_rom_hash_cache* c) { delete c; }

rn_status rnf_rom_hash_cache_sha256(rnf_rom_hash_cache* c, const char* path, int64_t size, double modified,
                                    char out_hex[65]) {
  if (!c || !path || !out_hex) return fail(RN_ERR_INVALID_ARG, "null argument");
  RNF_GUARD_BEGIN
  auto key = std::make_tuple(std::string(path), size, modified);
  {
    std::lock_guard<std::mutex> g(c->lock);
    auto it = c->cache.find(key);
    if (it != c->cache.end()) {
      std::memcpy(out_hex, it->second.c_str(), it->second.size() + 1);
      return RN_OK;
    }
  }
  char hex[65] = {0};
  rn_status st = rn_sha256_file(path, hex);
  if (st != RN_OK) return fail(st, rn_last_error());
  std::string h = lower(hex);
  {
    std::lock_guard<std::mutex> g(c->lock);
    c->cache[key] = h;
  }
  std::memcpy(out_hex, h.c_str(), std::min<size_t>(h.size(), 64) + 1);
  return RN_OK;
  RNF_GUARD_END(RN_ERR_INTERNAL)
}

rnf_game_match rnf_rom_hash_cache_identify(rnf_rom_hash_cache* c, const char* path, int64_t size, double modified,
                                           rnf_game_info* out) {
  if (!c || !path) return RNF_GAME_MATCH_NONE;
  try {
    auto key = std::make_tuple(std::string(path), size, modified);
    {
      std::lock_guard<std::mutex> g(c->lock);
      auto it = c->games.find(key);
      if (it != c->games.end()) {
        if (out && it->second.first != RNF_GAME_MATCH_NONE) *out = it->second.second;
        return it->second.first;
      }
    }
    rnf_game_info info{};
    rnf_game_match m = rnf_gamedb_identify_file(path, &info);
    {
      std::lock_guard<std::mutex> g(c->lock);
      c->games[key] = {m, info};
    }
    if (out && m != RNF_GAME_MATCH_NONE) *out = info;
    return m;
  } catch (...) {
    return RNF_GAME_MATCH_NONE;
  }
}

char* rnf_backup_path(const char* project_path, double date, rnf_exists_fn exists_fn, void* ctx) {
  if (!project_path) return nullptr;
  RNF_GUARD_BEGIN
  std::string stamp = localTime(date, "%Y-%m-%d %H.%M");
  std::string file = lastComponent(project_path);
  std::string name = stem(file);
  std::string ext = extensionOf(file);
  if (ext.empty()) ext = "nesrec";
  // Sibling of the project, spelled with the project path's own separator ("C:/x/a" stays "/").
  std::string trimmed = stripTrailing(project_path);
  size_t slash = lastPathSep(trimmed);
  std::string dir = slash == std::string::npos ? std::string(".") + kPathSep : trimmed.substr(0, slash + 1);
  std::string base = trf(RNF_L("%@ (Before Reset %@)"), {argS(name), argS(stamp)});
  for (int n = 1;; ++n) {
    std::string candidate = dir + (n == 1 ? base : base + " " + std::to_string(n)) + "." + ext;
    bool taken = exists_fn ? exists_fn(candidate.c_str(), ctx) != 0 : exists(candidate);
    if (!taken) return dup(candidate);
  }
  RNF_GUARD_END(nullptr)
}

}  // extern "C"
