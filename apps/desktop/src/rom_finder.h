// Find ROMs on the device's storage (Settings / the empty library > Add ROMs > Search the Device;
// docs/design/ANDROID.md, "Finding ROMs anywhere"): every folder under the roots, at any depth, hidden ones
// too, a ROM extension in any case; the files the library would play (the game database knows them) are
// offered, then copied into the ROM folder. A worker thread does the walk and the copy.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rnl {

struct FoundRom {
  std::string path, name;  // name: the file name
  int64_t size = 0;
};

struct RomSearch {
  std::vector<FoundRom> roms;      // playable, not in the ROM folder yet (one per file name)
  size_t alreadyThere = 0;         // playable, the same name and size already in the ROM folder
  size_t unknown = 0;              // a ROM extension the game database does not know (not offered)
  size_t folders = 0;              // folders read
  std::vector<std::string> skipped;  // folders that could not be read (Android/data of other apps ...)
};

class RomFinder {
 public:
  ~RomFinder();
  /// The walk: every folder under the roots except `romDir` (the library's own) and the paths in `skip`.
  void search(std::vector<std::string> roots, std::string romDir, std::vector<std::string> skip);
  /// The copy of the found ROMs into romDir: ".<name>.part", then renamed (the library never lists a half
  /// file); a file of the same name is replaced.
  void copy(std::vector<FoundRom> roms, std::string romDir);
  bool busy() const { return busy_.load(); }
  size_t foldersSeen() const { return folders_.load(); }
  size_t copied() const { return copied_.load(); }
  /// A finished search, once.
  bool takeSearch(RomSearch* out);
  /// A finished copy, once: how many went in, the names that failed.
  bool takeCopy(size_t* copied, std::vector<std::string>* failed);

  /// The walk itself (also for tests): no thread.
  static RomSearch walk(const std::vector<std::string>& roots, const std::string& romDir,
                        const std::vector<std::string>& skip, std::atomic<size_t>* folders);

 private:
  void join();
  std::thread worker_;
  std::atomic<bool> busy_{false};
  std::atomic<size_t> folders_{0}, copied_{0};
  std::mutex mutex_;
  bool searchDone_ = false, copyDone_ = false;
  RomSearch search_;
  std::vector<std::string> failed_;
};

}  // namespace rnl
