// Library card pictures: the last picture seen of each project and each ROM (a 128x120 thumbnail,
// ThumbImage), kept in <dir>/<hash>.thumb so the library shows them after a restart. Saved by the
// UI while a game is open (menu opened, pause, every half minute) on a worker thread; read lazily
// on the frame thread (one small file per card, cached).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "thumbnails.h"

namespace rnl {

class LibraryThumbs {
 public:
  explicit LibraryThumbs(std::string dir);
  ~LibraryThumbs();
  LibraryThumbs(const LibraryThumbs&) = delete;
  LibraryThumbs& operator=(const LibraryThumbs&) = delete;

  /// The picture (256x240 BGRA) as the thumbnail of a project and / or a ROM ("" = skip). Written
  /// off the calling thread; the in-memory copy is replaced at once.
  void save(const std::string& projectPath, const std::string& romPath, const uint32_t* frame256x240);
  /// Retained thumbnail, or empty when there is none.
  ThumbRef forProject(const std::string& projectPath) { return get("p:" + projectPath); }
  ThumbRef forRom(const std::string& romPath) { return get("r:" + romPath); }

  /// File of a key (tests).
  std::string fileFor(const std::string& key) const;

 private:
  ThumbRef get(const std::string& key);
  void put(const std::string& key, ThumbImage* img);  // takes a reference

  std::string dir_;
  std::map<std::string, ThumbImage*> cache_;  // nullptr: no file (looked up once)
  std::mutex writeMutex_;
  std::thread writer_;
};

}  // namespace rnl
