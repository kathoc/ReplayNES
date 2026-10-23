// SPDX-License-Identifier: GPL-2.0-or-later
#include "library_thumbs.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "replaynes/replaynes.h"

namespace rnl {

namespace {
constexpr char kMagic[8] = {'R', 'N', 'T', 'H', 'U', 'M', 'B', '1'};
constexpr size_t kPixels = size_t(RNF_THUMB_WIDTH) * RNF_THUMB_HEIGHT;

uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

bool writeFile(const std::string& path, const uint32_t* px) {
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(kMagic, sizeof kMagic);
    f.write(reinterpret_cast<const char*>(px), std::streamsize(kPixels * 4));
    if (!f) return false;
  }
  std::error_code ec;
  std::filesystem::rename(std::filesystem::path(tmp), std::filesystem::path(path), ec);
  return !ec;
}
}  // namespace

LibraryThumbs::LibraryThumbs(std::string dir) : dir_(std::move(dir)) {}

LibraryThumbs::~LibraryThumbs() {
  {
    std::lock_guard<std::mutex> lk(picMutex_);
    picStop_ = true;
    picQueue_.clear();
  }
  picCv_.notify_all();
  if (picThread_.joinable()) picThread_.join();
  if (writer_.joinable()) writer_.join();
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
    std::vector<uint32_t> px(kPixels);
    if (f && f.read(magic, sizeof magic) && std::memcmp(magic, kMagic, sizeof magic) == 0 &&
        f.read(reinterpret_cast<char*>(px.data()), std::streamsize(kPixels * 4)))
      img = ThumbImage::fromThumb(px.data());
    it = cache_.emplace(key, img).first;
  }
  if (!it->second) return {};
  it->second->retain();
  return ThumbRef(it->second);
}

void LibraryThumbs::save(const std::string& projectPath, const std::string& romPath, const uint32_t* frame) {
  if (!frame || (projectPath.empty() && romPath.empty())) return;
  ThumbImage* img = ThumbImage::make(frame);
  std::vector<std::pair<std::string, std::vector<uint32_t>>> writes;
  for (const std::string& key : {projectPath.empty() ? std::string() : "p:" + projectPath,
                                 romPath.empty() ? std::string() : "r:" + romPath}) {
    if (key.empty()) continue;
    img->retain();
    put(key, img);
    writes.emplace_back(fileFor(key), std::vector<uint32_t>(img->px, img->px + kPixels));
  }
  img->release();
  // One write at a time; a save while the previous one runs waits for it (they are seconds apart).
  if (writer_.joinable()) writer_.join();
  std::string dir = dir_;
  writer_ = std::thread([dir, writes = std::move(writes)] {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    for (const auto& [path, px] : writes)
      if (!writeFile(path, px.data())) std::fprintf(stderr, "cannot write %s\n", path.c_str());
  });
}

void LibraryThumbs::requestRomPicture(const std::string& romPath) {
  if (romPath.empty() || !asked_.insert(romPath).second) return;
  {
    std::lock_guard<std::mutex> lk(picMutex_);
    picQueue_.push_back(romPath);
  }
  if (!picThread_.joinable()) picThread_ = std::thread([this] { pictureWorker(); });
  picCv_.notify_one();
}

bool LibraryThumbs::pollRomPictures() {
  std::vector<std::pair<std::string, std::vector<uint32_t>>> done;
  {
    std::lock_guard<std::mutex> lk(picMutex_);
    done.swap(picDone_);
  }
  for (const auto& [rom, frame] : done) save("", rom, frame.data());
  return !done.empty();
}

void LibraryThumbs::pictureWorker() {
  for (;;) {
    std::string rom;
    {
      std::unique_lock<std::mutex> lk(picMutex_);
      picCv_.wait(lk, [this] { return picStop_ || !picQueue_.empty(); });
      if (picStop_) return;
      rom = std::move(picQueue_.front());
      picQueue_.pop_front();
    }
    // Power-on, no input, kRomPictureFrame frames: in memory only (no project directory).
    rn_session* s = nullptr;
    if (rn_session_new(rom.c_str(), nullptr, nullptr, &s) != RN_OK || !s) continue;
    bool ok = true;
    for (int f = 0; ok && f < kRomPictureFrame; ++f) {
      ok = rn_step(s, 0, 0, 0, nullptr) == RN_OK;
      std::lock_guard<std::mutex> lk(picMutex_);
      if (picStop_) ok = false;
    }
    const uint32_t* v = ok ? rn_video(s) : nullptr;
    if (v) {
      std::lock_guard<std::mutex> lk(picMutex_);
      picDone_.emplace_back(rom, std::vector<uint32_t>(v, v + 256 * 240));
    }
    rn_session_close(s);
  }
}

}  // namespace rnl
