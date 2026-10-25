// SPDX-License-Identifier: GPL-2.0-or-later
#include "replaynes/product.h"
#include "library_thumbs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "replaynes/replaynes.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rnl {

namespace {
constexpr char kMagic[8] = {'R', 'N', 'T', 'H', 'U', 'M', 'B', '1'};

uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

bool writeFile(const std::string& path, const std::vector<uint32_t>& px, uint32_t width, uint32_t height) {
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const bool legacy = width == RNF_THUMB_WIDTH && height == RNF_THUMB_HEIGHT;
    char magic[8];
    std::memcpy(magic, kMagic, sizeof magic);
    if (!legacy) magic[7] = '2';
    f.write(magic, sizeof magic);
    if (!legacy) {
      // Little-endian dimensions make the new cache format portable.
      for (uint32_t n : {width, height}) for (int shift = 0; shift < 32; shift += 8) f.put(char(n >> shift));
    }
    f.write(reinterpret_cast<const char*>(px.data()), std::streamsize(px.size() * 4));
    if (!f) return false;
  }
  std::error_code ec;
  std::filesystem::rename(std::filesystem::path(tmp), std::filesystem::path(path), ec);
  return !ec;
}

// The calling thread below the UI and the game (the picture worker).
void lowerThreadPriority() {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__linux__)
  setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 19);  // per thread on Linux
#endif
}
}  // namespace

LibraryThumbs::LibraryThumbs(std::string dir) : dir_(std::move(dir)) {}

LibraryThumbs::~LibraryThumbs() {
  {
    std::lock_guard<std::mutex> lk(picMutex_);
    picStop_ = true;
    picQueue_.clear();
    picLater_.clear();
  }
  picCv_.notify_all();
  if (picThread_.joinable()) picThread_.join();
  if (writer_.joinable()) writer_.join();
  for (auto& [rom, img] : picDone_) if (img) img->release();
  for (auto& [k, img] : cache_)
    if (img) img->release();
}

std::string LibraryThumbs::fileFor(const std::string& key) const {
  char name[32];
  std::snprintf(name, sizeof name, "%016llx.thumb", (unsigned long long)fnv1a(key));
  return dir_ + "/" + name;
}

void LibraryThumbs::put(const std::string& key, ThumbImage* img) {
  auto it = cache_.find(key);
  if (it != cache_.end() && it->second) it->second->release();
  cache_[key] = img;
}

ThumbRef LibraryThumbs::get(const std::string& key) {
  auto it = cache_.find(key);
  if (it == cache_.end()) {
    ThumbImage* img = nullptr;
    std::ifstream f(std::filesystem::path(fileFor(key)), std::ios::binary);
    char magic[8] = {};
    if (f && f.read(magic, sizeof magic) && std::memcmp(magic, kMagic, 7) == 0 &&
        (magic[7] == '1' || magic[7] == '2')) {
      uint32_t width = RNF_THUMB_WIDTH, height = RNF_THUMB_HEIGHT;
      if (magic[7] == '2') {
        uint32_t dims[2] = {};
        for (uint32_t& n : dims) for (int shift = 0; shift < 32; shift += 8) {
          int ch = f.get();
          if (ch == EOF) { f.setstate(std::ios::failbit); break; }
          n |= uint32_t(ch) << shift;
        }
        width = dims[0]; height = dims[1];
      }
      if (f && width == RNF_THUMB_WIDTH && height && height <= 65536) {
        std::vector<uint32_t> px(size_t(width) * height);
        if (f.read(reinterpret_cast<char*>(px.data()), std::streamsize(px.size() * 4)))
          img = ThumbImage::fromThumb(px.data(), width, height);
      }
    }
    it = cache_.emplace(key, img).first;
  }
  if (!it->second) return {};
  it->second->retain();
  return ThumbRef(it->second);
}

void LibraryThumbs::save(const std::string& projectPath, const std::string& romPath, const uint32_t* frame) {
  if (!frame || (projectPath.empty() && romPath.empty())) return;
  saveImage(projectPath, romPath, ThumbImage::make(frame));
}

void LibraryThumbs::save(const std::string& projectPath, const std::string& romPath,
                         const rn_video_frame_info& frame, const rn_media_info& media, bool nesPicture) {
  if (!frame.pixels || (projectPath.empty() && romPath.empty())) return;
  saveImage(projectPath, romPath, ThumbImage::make(frame, media, nesPicture));
}

void LibraryThumbs::saveImage(const std::string& projectPath, const std::string& romPath, ThumbImage* img) {
  if (!img) return;
  const uint32_t width = img->width, height = img->height;
  std::vector<std::pair<std::string, std::vector<uint32_t>>> writes;
  for (const std::string& key : {projectPath.empty() ? std::string() : "p:" + projectPath,
                                 romPath.empty() ? std::string() : "r:" + romPath}) {
    if (key.empty()) continue;
    img->retain();
    put(key, img);
    writes.emplace_back(fileFor(key), img->px);
  }
  img->release();
  // One write at a time; a save while the previous one runs waits for it (they are seconds apart).
  if (writer_.joinable()) writer_.join();
  std::string dir = dir_;
  writer_ = std::thread([dir, width, height, writes = std::move(writes)] {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    for (const auto& [path, px] : writes)
      if (!writeFile(path, px, width, height)) std::fprintf(stderr, "cannot write %s\n", path.c_str());
  });
}

void LibraryThumbs::startPictureWorker() {
  if (!picThread_.joinable()) picThread_ = std::thread([this] { pictureWorker(); });
  picCv_.notify_one();
}

void LibraryThumbs::requestRomPicture(const std::string& romPath) {
  if (romPath.empty()) return;
  auto [it, added] = asked_.emplace(romPath, true);
  if (!added && it->second) return;
  it->second = true;
  {
    std::lock_guard<std::mutex> lk(picMutex_);
    if (!added) {  // queued for later: now, unless the worker has it already
      auto later = std::find(picLater_.begin(), picLater_.end(), romPath);
      if (later == picLater_.end()) return;
      picLater_.erase(later);
    }
    picQueue_.push_back(romPath);
  }
  startPictureWorker();
}

void LibraryThumbs::requestRomPictureLater(const std::string& romPath) {
  if (romPath.empty() || !asked_.emplace(romPath, false).second) return;
  {
    std::lock_guard<std::mutex> lk(picMutex_);
    picLater_.push_back(romPath);
  }
  startPictureWorker();
}

void LibraryThumbs::setPicturesPaused(bool paused) {
  {
    std::lock_guard<std::mutex> lk(picMutex_);
    if (picPaused_ == paused) return;
    picPaused_ = paused;
  }
  picCv_.notify_all();
}

bool LibraryThumbs::pollRomPictures() {
  std::vector<std::pair<std::string, ThumbImage*>> done;
  {
    std::lock_guard<std::mutex> lk(picMutex_);
    done.swap(picDone_);
  }
  // On disk already (the worker wrote it): a card that looked for the picture gets it now, any
  // other ROM's is read when its card shows.
  for (const auto& [rom, img] : done) {
    if (cache_.count("r:" + rom)) put("r:" + rom, img);
    else img->release();
  }
  return !done.empty();
}

void LibraryThumbs::pictureWorker() {
  lowerThreadPriority();
  for (;;) {
    std::string rom;
    bool later = false;
    {
      std::unique_lock<std::mutex> lk(picMutex_);
      picCv_.wait(lk, [this] { return picStop_ || (!picPaused_ && (!picQueue_.empty() || !picLater_.empty())); });
      if (picStop_) return;
      std::deque<std::string>& q = picQueue_.empty() ? picLater_ : picQueue_;
      later = &q == &picLater_;
      rom = std::move(q.front());
      q.pop_front();
    }
    std::error_code ec;
    if (later && std::filesystem::exists(std::filesystem::path(fileFor("r:" + rom)), ec)) continue;
    // Power-on, no input, kRomPictureFrame frames: in memory only (no project directory).
    rn_session* s = nullptr;
    // The product's default core (an external core reads files the NES cores cannot).
    rn_session_options opt;
    rn_session_options_init(&opt);
    const bool external = std::strcmp(RN_PRODUCT_DEFAULT_CORE, "external") == 0;
    if (external) opt.core = RN_CORE_EXTERNAL;
    if (rn_session_new(rom.c_str(), nullptr, external ? &opt : nullptr, &s) != RN_OK || !s) continue;
    bool ok = true;
    for (int f = 0; ok && f < kRomPictureFrame; ++f) {
      const auto t0 = std::chrono::steady_clock::now();
      ok = rn_step(s, 0, 0, 0, nullptr) == RN_OK;
      std::unique_lock<std::mutex> lk(picMutex_);
      if (picStop_) {
        ok = false;
      } else if (picPaused_) {  // a game opened: again from power-on once it closes
        ok = false;
        (later ? picLater_ : picQueue_).push_front(rom);
      } else if (later && ok) {
        // As long a rest as the frame took: half of one core at most. Woken by a pause or stop.
        picCv_.wait_for(lk, std::chrono::steady_clock::now() - t0, [this] { return picStop_ || picPaused_; });
      }
    }
    rn_video_frame_info frame{};
    rn_media_info media{};
    if (ok && rn_session_video_frame(s, &frame) == RN_OK && rn_session_media_info(s, &media) == RN_OK) {
      if (ThumbImage* img = ThumbImage::make(frame, media, rn_session_core_kind(s) != RN_CORE_EXTERNAL)) {
        // Written before it is announced, so a card never looks for a file still to come.
        std::filesystem::create_directories(std::filesystem::path(dir_), ec);
        const std::string path = fileFor("r:" + rom);
        if (!writeFile(path, img->px, img->width, img->height)) std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::lock_guard<std::mutex> lk(picMutex_);
        picDone_.emplace_back(rom, img);
      }
    }
    rn_session_close(s);
  }
}

}  // namespace rnl
