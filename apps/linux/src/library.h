// ROM library state (LibraryModel.swift on macOS): <root>/ROM and <root>/Projects are created at
// launch; scans run on a background thread (ROM SHA-256 via the core's hash cache, project
// manifests) and are swapped in on the frame thread by poll(). Folders are re-checked every few
// seconds while the library is on screen (modification times only), so files copied in from
// Desktop Mode appear without a restart.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "replaynes/frontend.h"

namespace rnl {

struct LibraryROM {
  std::string path, name, relativePath;
  int64_t size = 0;
  double modified = 0;
  std::string sha256;  // "" when it can't be computed
};

struct LibraryProject {
  std::string path, name, romSHA256, romName;
  double modified = 0;
};

/// Case-insensitive (ASCII + full-width ASCII) substring match of a search query against a ROM's
/// name or relative path; an empty query matches everything.
bool librarySearchMatches(const std::string& query, const LibraryROM& rom);

class LibraryModel {
 public:
  explicit LibraryModel(std::string root);
  ~LibraryModel();

  const std::string& root() const { return root_; }
  std::string romDir() const;
  std::string projectsDir() const;

  /// Creates ROM/ and Projects/. Returns false with folderError() set.
  bool ensureFolders();
  const std::string& folderError() const { return folderError_; }

  /// Starts a background rescan (coalesced).
  void refresh();
  /// Frame thread: takes finished scan results. Returns true when roms/projects changed.
  bool poll();
  /// While the library is visible: rescans when a folder's modification time changed (~every 3 s).
  void watch(double now);

  bool scanning() const { return scanning_.load(); }
  const std::string& scanError() const { return scanError_; }
  const std::vector<LibraryROM>& roms() const { return roms_; }
  std::vector<LibraryProject> projectsFor(const LibraryROM& rom) const;
  const std::vector<LibraryProject>& allProjects() const { return projects_; }
  uint64_t version() const { return version_; }

 private:
  struct Result {
    std::vector<LibraryROM> roms;
    std::vector<LibraryProject> projects;
    std::string error;
  };
  void scanThread();
  std::map<std::string, double> folderTimes() const;

  std::string root_;
  std::string folderError_;
  rnf_rom_hash_cache* hashes_ = nullptr;
  std::thread worker_;
  std::atomic<bool> scanning_{false};
  std::atomic<bool> again_{false};
  std::mutex mutex_;
  bool hasResult_ = false;
  Result result_;
  std::vector<LibraryROM> roms_;
  std::vector<LibraryProject> projects_;
  std::map<std::string, std::vector<size_t>> bySHA_;
  std::string scanError_;
  uint64_t version_ = 0;
  double lastWatch_ = 0;
  std::map<std::string, double> times_;
};

}  // namespace rnl
