// SPDX-License-Identifier: GPL-2.0-or-later
#include "library.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "l10n.h"

namespace fs = std::filesystem;

namespace rnl {

namespace {
/// Lower-cases ASCII and folds full-width ASCII (U+FF01..U+FF5E) to ASCII, byte-wise UTF-8.
std::string fold(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == 0xEF && i + 2 < s.size()) {
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]), c2 = static_cast<unsigned char>(s[i + 2]);
      unsigned cp = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
      if (cp >= 0xFF01 && cp <= 0xFF5E) {
        char a = char(cp - 0xFF01 + 0x21);
        out += char(std::tolower(static_cast<unsigned char>(a)));
        i += 2;
        continue;
      }
    }
    out += char(c < 0x80 ? std::tolower(c) : c);
  }
  return out;
}
/// Change detection only (folder modification times compared with themselves).
double mtime(const std::string& p) {
#ifdef _WIN32
  std::error_code ec;
  auto t = fs::last_write_time(p, ec);
  return ec ? -1 : double(t.time_since_epoch().count());
#else
  struct stat st{};
  if (::stat(p.c_str(), &st) != 0) return -1;
#ifdef __APPLE__
  return double(st.st_mtimespec.tv_sec) + double(st.st_mtimespec.tv_nsec) * 1e-9;
#else
  return double(st.st_mtim.tv_sec) + double(st.st_mtim.tv_nsec) * 1e-9;
#endif
#endif
}
}  // namespace

bool librarySearchMatches(const std::string& query, const LibraryROM& rom) {
  std::string q = fold(query);
  size_t a = q.find_first_not_of(' ');
  if (a == std::string::npos) return true;
  q = q.substr(a, q.find_last_not_of(' ') - a + 1);
  return fold(rom.name).find(q) != std::string::npos || fold(rom.relativePath).find(q) != std::string::npos;
}

namespace {
std::string takeString(char* s) {
  std::string out = s ? s : "";
  rnf_string_free(s);
  return out;
}
}  // namespace

std::string LibraryROM::title() const { return takeString(rnf_game_title(info(), name.c_str())); }
std::string LibraryROM::byline() const { return takeString(rnf_game_byline(info())); }
std::string LibraryROM::details() const { return takeString(rnf_game_details(info())); }

LibraryModel::LibraryModel(std::string root)
    : root_(std::move(root)), hashes_(rnf_rom_hash_cache_new()), prefs_(rnf_library_prefs_new()) {}

LibraryModel::~LibraryModel() {
  if (worker_.joinable()) worker_.join();
  rnf_rom_hash_cache_free(hashes_);
  rnf_library_prefs_free(prefs_);
}

// ------------------------------------------------------------------ favourites / history / order

void LibraryModel::setPrefsFile(const std::string& path) {
  prefsFile_ = path;
  if (rnf_library_prefs_load(prefs_, path.c_str()) != RN_OK)
    std::fprintf(stderr, "library prefs: %s (starting empty)\n", rnf_last_error());
  prefsVersion_ += 1;
}

void LibraryModel::savePrefs() {
  prefsVersion_ += 1;
  if (prefsFile_.empty()) return;
  if (rnf_library_prefs_save(prefs_, prefsFile_.c_str()) != RN_OK) std::fprintf(stderr, "library prefs: %s\n", rnf_last_error());
}

const LibraryROM* LibraryModel::romWithSHA(const std::string& sha256) const {
  if (sha256.empty()) return nullptr;
  for (const LibraryROM& r : roms_)
    if (r.sha256 == sha256) return &r;
  return nullptr;
}

bool LibraryModel::isFavorite(const LibraryROM& rom) const { return rnf_library_prefs_is_favorite(prefs_, rom.sha256.c_str()) != 0; }

bool LibraryModel::toggleFavorite(const LibraryROM& rom) {
  if (rom.sha256.empty()) return false;
  bool on = rnf_library_prefs_toggle_favorite(prefs_, rom.sha256.c_str()) != 0;
  savePrefs();
  return on;
}

void LibraryModel::recordPlay(const std::string& sha256, double start, double seconds) {
  if (sha256.empty()) return;
  rnf_library_prefs_record_play(prefs_, sha256.c_str(), start, seconds);
  savePrefs();
}

bool LibraryModel::history(const LibraryROM& rom, rnf_library_history_entry* out) const {
  return rnf_library_prefs_history_find(prefs_, rom.sha256.c_str(), out) != 0;
}

void LibraryModel::setSort(rnf_library_sort s) {
  rnf_library_prefs_set_sort(prefs_, s);
  savePrefs();
}

void LibraryModel::setFilter(rnf_library_filter f) {
  rnf_library_prefs_set_filter(prefs_, f);
  savePrefs();
}

const std::vector<const LibraryROM*>& LibraryModel::arranged(const std::string& query) const {
  std::string key = std::to_string(version_) + "/" + std::to_string(prefsVersion_) + "/" + rnf_l10n_language() + "/" + query;
  if (key == arrangedKey_) return arranged_;
  std::vector<rnf_library_item> items;
  items.reserve(roms_.size());
  for (const LibraryROM& r : roms_) items.push_back({r.sha256.c_str(), r.name.c_str(), r.relativePath.c_str(), r.info()});
  size_t n = rnf_library_arrange(items.data(), items.size(), sort(), filter(), query.c_str(), prefs_, nullptr, 0);
  std::vector<size_t> order(n);
  rnf_library_arrange(items.data(), items.size(), sort(), filter(), query.c_str(), prefs_, order.data(), order.size());
  arranged_.clear();
  for (size_t i : order) arranged_.push_back(&roms_[i]);
  arrangedKey_ = key;
  return arranged_;
}

std::string LibraryModel::romDir() const { return root_ + "/" + RNF_LIBRARY_ROM_DIR; }
std::string LibraryModel::projectsDir() const { return root_ + "/" + RNF_LIBRARY_PROJECTS_DIR; }

bool LibraryModel::ensureFolders() {
  char* failed = nullptr;
  rn_status st = rnf_library_ensure(root_.c_str(), &failed);
  if (st == RN_OK) {
    folderError_.clear();
  } else {
    std::string where = failed ? failed : root_;
    folderError_ = st == RN_ERR_ALREADY_EXISTS ? TRF("A file with the same name is where the folder should be: %@", {where})
                                               : TRF("Can’t create the folder: %@\n%@", {where, std::string(rnf_last_error())});
  }
  rnf_string_free(failed);
  return st == RN_OK;
}

std::map<std::string, double> LibraryModel::folderTimes() const {
  std::map<std::string, double> t;
  t[romDir()] = mtime(romDir());
  t[projectsDir()] = mtime(projectsDir());
  std::error_code ec;
  for (fs::directory_iterator it(romDir(), ec), end; !ec && it != end; it.increment(ec))
    if (it->is_directory(ec)) t[it->path().string()] = mtime(it->path().string());
  return t;
}

void LibraryModel::refresh() {
  if (scanning_.load()) {
    again_ = true;
    return;
  }
  if (worker_.joinable()) worker_.join();
  if (!folderError_.empty()) ensureFolders();
  scanning_ = true;
  times_ = folderTimes();
  worker_ = std::thread([this] { scanThread(); });
}

void LibraryModel::scanThread() {
  for (;;) {
    again_ = false;
    Result r;
    rnf_rom_list* roms = nullptr;
    if (rnf_library_scan_roms(romDir().c_str(), nullptr, nullptr, &roms) == RN_OK) {
      for (size_t i = 0, n = rnf_rom_list_count(roms); i < n; ++i) {
        rnf_rom_entry e{};
        if (!rnf_rom_list_get(roms, i, &e)) continue;
        LibraryROM rom{e.path, e.name, e.relative_path, e.size, e.modified, ""};
        char hex[65] = {0};
        if (rnf_rom_hash_cache_sha256(hashes_, e.path, e.size, e.modified, hex) == RN_OK) rom.sha256 = hex;
        rom.known = rnf_rom_hash_cache_identify(hashes_, e.path, e.size, e.modified, &rom.game) != RNF_GAME_MATCH_NONE;
        r.roms.push_back(std::move(rom));
      }
      rnf_rom_list_free(roms);
    } else {
      r.error = TRF("Can’t read the folder: %@\n%@", {romDir(), std::string(rnf_last_error())});
    }
    rnf_project_list* projects = nullptr;
    if (rnf_library_scan_projects(projectsDir().c_str(), &projects) == RN_OK) {
      for (size_t i = 0, n = rnf_project_list_count(projects); i < n; ++i) {
        rnf_project_entry e{};
        if (!rnf_project_list_get(projects, i, &e)) continue;
        r.projects.push_back(LibraryProject{e.path, e.name, e.rom_sha256, e.rom_name, e.modified});
      }
      rnf_project_list_free(projects);
    } else {
      std::string err = TRF("Can’t read the folder: %@\n%@", {projectsDir(), std::string(rnf_last_error())});
      r.error = r.error.empty() ? err : r.error + "\n" + err;
    }
    {
      std::lock_guard<std::mutex> lk(mutex_);
      result_ = std::move(r);
      hasResult_ = true;
    }
    if (!again_.load()) break;
  }
  scanning_ = false;
}

bool LibraryModel::poll() {
  Result r;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!hasResult_) return false;
    hasResult_ = false;
    r = std::move(result_);
  }
  scanError_ = r.error;
  bool changed = r.roms.size() != roms_.size() || r.projects.size() != projects_.size();
  for (size_t i = 0; !changed && i < r.roms.size(); ++i)
    changed = r.roms[i].path != roms_[i].path || r.roms[i].sha256 != roms_[i].sha256 || r.roms[i].known != roms_[i].known;
  for (size_t i = 0; !changed && i < r.projects.size(); ++i)
    changed = r.projects[i].path != projects_[i].path || r.projects[i].modified != projects_[i].modified;
  roms_ = std::move(r.roms);
  arrangedKey_.clear();  // the cached order points into roms_
  projects_ = std::move(r.projects);
  bySHA_.clear();
  for (size_t i = 0; i < projects_.size(); ++i) bySHA_[projects_[i].romSHA256].push_back(i);
  if (changed) version_ += 1;
  return changed;
}

std::vector<LibraryProject> LibraryModel::projectsFor(const LibraryROM& rom) const {
  std::vector<LibraryProject> out;
  if (rom.sha256.empty()) return out;
  auto it = bySHA_.find(rom.sha256);
  if (it == bySHA_.end()) return out;
  for (size_t i : it->second) out.push_back(projects_[i]);
  return out;
}

void LibraryModel::watch(double now) {
  if (now - lastWatch_ < 3.0) return;
  lastWatch_ = now;
  if (scanning_.load()) return;
  if (folderTimes() != times_) refresh();
}

}  // namespace rnl
