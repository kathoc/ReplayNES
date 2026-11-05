// RomFinder (rom_finder.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "rom_finder.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>

#include "replaynes/frontend.h"

namespace rnl {
namespace fs = std::filesystem;

namespace {
bool samePath(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (fs::equivalent(a, b, ec)) return true;
  return a.lexically_normal() == b.lexically_normal();
}
}  // namespace

RomFinder::~RomFinder() { join(); }

void RomFinder::join() {
  if (worker_.joinable()) worker_.join();
}

RomSearch RomFinder::walk(const std::vector<std::string>& roots, const std::string& romDir,
                          const std::vector<std::string>& skip, std::atomic<size_t>* folders) {
  RomSearch r;
  const fs::path rom = fs::u8path(romDir);
  std::vector<fs::path> skipPaths;
  for (const std::string& s : skip) skipPaths.push_back(fs::u8path(s));
  std::map<std::string, size_t> byName;  // file name -> index in r.roms
  std::vector<fs::path> stack;
  for (const std::string& root : roots) stack.push_back(fs::u8path(root));
  while (!stack.empty()) {
    const fs::path dir = stack.back();
    stack.pop_back();
    if (samePath(dir, rom)) continue;
    bool skipped = false;
    for (const fs::path& s : skipPaths) skipped = skipped || samePath(dir, s);
    if (skipped) continue;
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) {
      r.skipped.push_back(dir.u8string());
      continue;
    }
    r.folders += 1;
    if (folders) folders->store(r.folders);
    for (; it != end; it.increment(ec)) {
      if (ec) break;
      std::error_code fe;
      const fs::file_status st = it->symlink_status(fe);
      if (fe || fs::is_symlink(st)) continue;  // a link may loop; its target is walked where it is
      const fs::path p = it->path();
      if (fs::is_directory(st)) {
        stack.push_back(p);
        continue;
      }
      if (!fs::is_regular_file(st)) continue;
      const std::string path = p.u8string();
      if (!rnf_library_is_rom_file(path.c_str())) continue;
      rnf_game_info info{};
      if (rnf_gamedb_identify_file(path.c_str(), &info) == RNF_GAME_MATCH_NONE) {
        r.unknown += 1;
        continue;
      }
      FoundRom f;
      f.path = path;
      f.name = p.filename().u8string();
      f.size = int64_t(it->file_size(fe));
      // Already in the ROM folder: the same name and size.
      std::error_code xe;
      const fs::path there = rom / p.filename();
      if (fs::exists(there, xe) && int64_t(fs::file_size(there, xe)) == f.size) {
        r.alreadyThere += 1;
        continue;
      }
      // One per name (the ROM folder holds one): the largest copy (a complete set rather than a split one).
      auto at = byName.find(f.name);
      if (at == byName.end()) {
        byName[f.name] = r.roms.size();
        r.roms.push_back(f);
      } else if (f.size > r.roms[at->second].size) {
        r.roms[at->second] = f;
      }
    }
  }
  std::sort(r.roms.begin(), r.roms.end(), [](const FoundRom& a, const FoundRom& b) { return a.name < b.name; });
  return r;
}

void RomFinder::search(std::vector<std::string> roots, std::string romDir, std::vector<std::string> skip) {
  if (busy_.load()) return;
  join();
  busy_ = true;
  folders_ = 0;
  worker_ = std::thread([this, roots = std::move(roots), romDir = std::move(romDir), skip = std::move(skip)] {
    RomSearch r = walk(roots, romDir, skip, &folders_);
    {
      std::lock_guard<std::mutex> lk(mutex_);
      search_ = std::move(r);
      searchDone_ = true;
    }
    busy_ = false;
  });
}

void RomFinder::copy(std::vector<FoundRom> roms, std::string romDir) {
  if (busy_.load()) return;
  join();
  busy_ = true;
  copied_ = 0;
  worker_ = std::thread([this, roms = std::move(roms), romDir = std::move(romDir)] {
    std::vector<std::string> failed;
    std::error_code ec;
    fs::create_directories(fs::u8path(romDir), ec);
    for (const FoundRom& f : roms) {
      const fs::path dest = fs::u8path(romDir) / fs::u8path(f.name);
      const fs::path part = fs::u8path(romDir) / fs::u8path("." + f.name + ".part");
      std::error_code e;
      fs::copy_file(fs::u8path(f.path), part, fs::copy_options::overwrite_existing, e);
      if (!e) fs::rename(part, dest, e);
      if (e) {
        fs::remove(part, ec);
        failed.push_back(f.name);
        std::fprintf(stderr, "find: copy %s: %s\n", f.path.c_str(), e.message().c_str());
        continue;
      }
      copied_ += 1;
      std::fprintf(stderr, "find: %s -> %s\n", f.path.c_str(), dest.u8string().c_str());
    }
    {
      std::lock_guard<std::mutex> lk(mutex_);
      failed_ = std::move(failed);
      copyDone_ = true;
    }
    busy_ = false;
  });
}

bool RomFinder::takeSearch(RomSearch* out) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!searchDone_) return false;
  searchDone_ = false;
  *out = std::move(search_);
  return true;
}

bool RomFinder::takeCopy(size_t* copied, std::vector<std::string>* failed) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!copyDone_) return false;
  copyDone_ = false;
  *copied = copied_.load();
  *failed = std::move(failed_);
  return true;
}

}  // namespace rnl
