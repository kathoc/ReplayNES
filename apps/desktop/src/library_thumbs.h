// Library card pictures: the last picture seen of each project and each ROM (a 128-pixel-wide thumbnail,
// ThumbImage), kept in <dir>/<hash>.thumb so the library shows them after a restart. Saved by the
// UI while a game is open (menu opened, pause, every half minute) on a worker thread; read lazily
// on the frame thread (one small file per card, cached).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "replaynes/product.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
  void save(const std::string& projectPath, const std::string& romPath,
            const rn_video_frame_info& frame, const rn_media_info& media, bool nesPicture = false);
  /// Retained thumbnail, or empty when there is none.
  ThumbRef forProject(const std::string& projectPath) { return get("p:" + projectPath); }
  ThumbRef forRom(const std::string& romPath) { return get("r:" + romPath); }

  /// A ROM with no picture yet: a worker thread emulates it from power-on for kRomPictureFrame
  /// frames (more while the picture is blank, see below) without input (the engine, no project, nothing written but the thumbnail) and that
  /// picture becomes the ROM's thumbnail. Asked by the library for the cards it shows; one ROM at a
  /// time, each ROM at most once per run (a ROM the engine cannot run stays a name tile).
  static constexpr int kRomPictureFrame = RN_PRODUCT_LIBRARY_PICTURE_FRAME;  // 300 for ReplayNES
  /// A picture that is blank (black, or one colour: a game still starting) waits kRomPictureFrame
  /// frames more, up to kRomPictureMaxFrame; the last one is kept in any case.
  static constexpr int kRomPictureMaxFrame = 6 * kRomPictureFrame;
  /// Almost one colour: under 5 % of the pixels differ clearly from the commonest colour.
  static bool blankPicture(const ThumbImage& img);
  void requestRomPicture(const std::string& romPath);
  /// The same for a ROM not on screen, after every shown card's picture: skipped when its file
  /// already exists, run at a low thread priority and at most half of one core's time, and kept
  /// on disk only (the card reads it when it is shown). A later requestRomPicture moves it ahead.
  void requestRomPictureLater(const std::string& romPath);
  /// While a game is open: no picture is made (a run in progress is put back in its queue), so the
  /// game has the machine to itself - the core may share one emulated machine between sessions.
  void setPicturesPaused(bool paused);
  /// Frame thread: takes the finished pictures (their files are written by then) into the cache of
  /// the cards that asked. True if any arrived.
  bool pollRomPictures();

  /// File of a key (tests).
  std::string fileFor(const std::string& key) const;

 private:
  ThumbRef get(const std::string& key);
  void saveImage(const std::string& projectPath, const std::string& romPath, ThumbImage* img);
  void put(const std::string& key, ThumbImage* img);  // takes a reference

  std::string dir_;
  std::map<std::string, ThumbImage*> cache_;  // nullptr: no file (looked up once)
  std::mutex writeMutex_;
  std::thread writer_;
  // ROM pictures (requestRomPicture / requestRomPictureLater): queues, results and the worker.
  void pictureWorker();
  void startPictureWorker();
  std::map<std::string, bool> asked_;  // frame thread only: ROM -> asked for a shown card
  std::mutex picMutex_;
  std::condition_variable picCv_;
  std::deque<std::string> picQueue_;  // shown cards first
  std::deque<std::string> picLater_;  // then the rest
  std::vector<std::pair<std::string, ThumbImage*>> picDone_;
  bool picStop_ = false;
  bool picPaused_ = false;
  std::thread picThread_;
};

}  // namespace rnl
