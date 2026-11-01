// SPDX-License-Identifier: GPL-2.0-or-later
#include "thumbnails.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "qoi.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#include <algorithm>
#include <cstring>

namespace rnl {

namespace {
std::atomic<uint64_t> gSerial{1};
void retainFn(void* p) { static_cast<ThumbImage*>(p)->retain(); }
void releaseFn(void* p) { static_cast<ThumbImage*>(p)->release(); }
constexpr int kRenderersPerPump = 8;
constexpr double kFadeDuration = 0.3;
}  // namespace

ThumbImage* ThumbImage::make(const uint32_t* frame) {
  const rn_video_frame_info f{frame, RN_VIDEO_WIDTH, RN_VIDEO_HEIGHT, RN_VIDEO_WIDTH};
  const rn_media_info m{RN_VIDEO_WIDTH, RN_VIDEO_HEIGHT, RN_FPS_NUM, RN_FPS_DEN, 1, 1, 1, 0};
  return make(f, m);
}

namespace {
rn_media_info thumbnailMedia(rn_media_info m, bool nesPicture) {
  // NES thumbnails historically use square pixels, independently of the 8:7 display toggle.
  if (nesPicture && m.max_width == RN_VIDEO_WIDTH && m.max_height == RN_VIDEO_HEIGHT && m.rotation == 0 &&
      m.pixel_aspect_num == 8 && m.pixel_aspect_den == 7)
    m.pixel_aspect_num = m.pixel_aspect_den = 1;
  return m;
}
}

bool thumbnailSize(const rn_video_frame_info& frame, const rn_media_info& media, uint32_t* width, uint32_t* height, bool nesPicture) {
  const rn_media_info m = thumbnailMedia(media, nesPicture);
  uint32_t w = 0, h = 0;
  if (!width || !height || !rnf_thumb_size(&frame, &m, &w, &h)) return false;
  *width = w; *height = h;
  return true;
}

ThumbImage* ThumbImage::make(const rn_video_frame_info& frame, const rn_media_info& media, bool nesPicture) {
  const rn_media_info m = thumbnailMedia(media, nesPicture);
  uint32_t w = 0, h = 0;
  if (!rnf_thumb_size(&frame, &m, &w, &h)) return nullptr;
  auto* t = new ThumbImage;
  t->serial = gSerial.fetch_add(1);
  t->width = w;
  t->height = h;
  t->px.resize(size_t(w) * h);
  if (!rnf_thumb_downscale_frame(&frame, &m, t->px.data(), t->px.size())) { delete t; return nullptr; }
  return t;
}

ThumbImage* ThumbImage::fromThumb(const uint32_t* px, uint32_t width, uint32_t height) {
  if (!px || !width || !height) return nullptr;
  auto* t = new ThumbImage;
  t->serial = gSerial.fetch_add(1);
  t->width = width;
  t->height = height;
  t->px.assign(px, px + size_t(width) * height);
  return t;
}

ThumbnailManager::ThumbnailManager() : cache_(rnf_thumb_cache_new(&retainFn, &releaseFn)) {
  unsigned hw = std::thread::hardware_concurrency();
  int n = int(std::max(1u, std::min(3u, hw > 2 ? hw - 2 : 1u)));
  resizeWorkers(unsigned(n));
}

void ThumbnailManager::resizeWorkers(unsigned count) {
  if (workers_.size() == count) return;
  {
    std::lock_guard<std::mutex> lk(qMutex_);
    stop_ = true;
  }
  qCv_.notify_all();
  for (auto& worker : workers_) worker.join();
  workers_.clear();
  for (Task& task : queue_) {
    rn_renderer_free(task.renderer);
    finishOne(task.job, task.frame, nullptr, false);
    --inFlight_;
  }
  queue_.clear();
  stop_ = false;
  for (unsigned i = 0; i < count; ++i) workers_.emplace_back([this] { workerLoop(); });
}

ThumbnailManager::~ThumbnailManager() {
  {
    std::lock_guard<std::mutex> lk(qMutex_);
    stop_ = true;
    if (job_) job_->cancelled = true;
  }
  qCv_.notify_all();
  for (auto& t : workers_) t.join();
  for (Task& t : queue_) rn_renderer_free(t.renderer);
  finished_.clear();
  for (Ready& r : ready_) r.image->release();
  clearStore();
  rnf_thumb_cache_free(cache_);
}

void ThumbnailManager::workerLoop() {
  // Below emulation and display: never compete with the frame loop.
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
#elif defined(SCHED_IDLE)
  sched_param sp{};
  pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
#endif
  for (;;) {
    Task t;
    {
      std::unique_lock<std::mutex> lk(qMutex_);
      qCv_.wait(lk, [&] { return stop_ || (!queue_.empty() && (!sharesMachine_ || !coreBusy_)); });
      if (stop_) return;
      t = queue_.front();
      queue_.pop_front();
    }
    ThumbImage* img = nullptr;
    if (!t.job->cancelled.load()) {
      const uint32_t* video = nullptr;
      const int16_t* audio = nullptr;
      size_t n = 0;
      uint64_t idx = 0;
      if (rn_renderer_next(t.renderer, &video, &audio, &n, &idx) == RN_OK && idx + 1 == t.frame && video) {
        rn_video_frame_info frame{};
        rn_media_info media{};
        if (rn_renderer_video_frame(t.renderer, &frame) == RN_OK && rn_renderer_media_info(t.renderer, &media) == RN_OK)
          img = ThumbImage::make(frame, media, t.nesPicture);
      }
    }
    rn_renderer_free(t.renderer);
    finishOne(t.job, t.frame, img, img == nullptr && !t.job->cancelled.load());
    --inFlight_;
  }
}

void ThumbnailManager::finishOne(const std::shared_ptr<Job>& j, uint64_t frame, ThumbImage* img, bool failed) {
  if (img) {  // shown at the next pump, not when the whole batch is done
    std::lock_guard<std::mutex> lk(qMutex_);
    ready_.push_back(Ready{j->take, j->generation, frame, img});
  }
  if (failed) j->failed = true;
  if (j->pending.fetch_sub(1) == 1) {
    std::lock_guard<std::mutex> lk(qMutex_);
    finished_.push_back(j);
  }
}

void ThumbnailManager::sessionInstalled(rn_session* s) {
  if (job_) job_->cancelled = true;
  job_.reset();
  {
    std::lock_guard<std::mutex> lk(qMutex_);
    sharesMachine_ = s && (rn_core_capabilities(rn_session_core_kind(s)) & RN_CORE_CAP_SHARES_MACHINE);
    coreBusy_ = false;
  }
  unsigned hw = std::thread::hardware_concurrency();
  resizeWorkers(sharesMachine_ ? 1u : std::max(1u, std::min(3u, hw > 2 ? hw - 2 : 1u)));
  rnf_thumb_cache_reset(cache_, s ? rn_active_take(s) : 0);
  if (step_ > 0) rnf_thumb_cache_set_step(cache_, step_);
  hasTarget_ = false;
  nextReconcile_ = 0;
  clearStore();
  if (s && enabled) loadStore(s);
}

void ThumbnailManager::sessionClosing(rn_session* s) {
  if (enabled) saveStore(s);
  clearStore();
}

namespace {
constexpr char kStoreMagic[8] = {'R', 'N', 'F', 'I', 'L', 'M', '1', '\n'};

uint64_t fnv(const std::string& text, uint64_t h = 1469598103934665603ull) {
  for (unsigned char c : text) h = (h ^ c) * 1099511628211ull;
  return h;
}

// The project's file: its folder and core (the same take may look different under another core).
std::string storeFileFor(const std::string& dir, rn_session* s) {
  const char* project = s ? rn_session_project_dir(s) : nullptr;
  if (dir.empty() || !project || !*project) return {};
  char name[40];
  std::snprintf(name, sizeof name, "%016llx.filmstrip",
                static_cast<unsigned long long>(fnv(std::to_string(int(rn_session_core_kind(s))), fnv(project))));
  return dir + "/" + name;
}

template <typename T>
bool readValue(std::istream& in, T* v) {
  return bool(in.read(reinterpret_cast<char*>(v), sizeof *v));
}
template <typename T>
void writeValue(std::ostream& out, T v) {
  out.write(reinterpret_cast<const char*>(&v), sizeof v);
}
}  // namespace

void ThumbnailManager::clearStore() {
  for (auto& [hash, st] : stored_) st.image->release();
  stored_.clear();
  storeFile_.clear();
  storeDirty_ = false;
}

// File: the magic, a count, then per picture its input hash, frame, size and QOI bytes.
void ThumbnailManager::loadStore(rn_session* s) {
  storeFile_ = storeFileFor(storeDir, s);
  if (storeFile_.empty()) return;
  std::ifstream in(storeFile_, std::ios::binary);
  char magic[8] = {};
  uint32_t count = 0;
  if (!in.read(magic, 8) || std::memcmp(magic, kStoreMagic, 8) != 0 || !readValue(in, &count)) return;
  std::vector<uint8_t> qoi, rgba;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t hash = 0, frame = 0;
    uint32_t size = 0, w = 0, h = 0;
    if (!readValue(in, &hash) || !readValue(in, &frame) || !readValue(in, &size) || size > (1u << 24)) break;
    qoi.resize(size);
    if (!in.read(reinterpret_cast<char*>(qoi.data()), size)) break;
    if (!decodeQoi(qoi.data(), qoi.size(), &w, &h, &rgba) || w > 1024 || h > 1024) continue;
    ThumbImage* img = ThumbImage::fromThumb(reinterpret_cast<const uint32_t*>(rgba.data()), w, h);
    if (!img) continue;
    auto [it, fresh] = stored_.insert({hash, Stored{frame, img}});
    if (!fresh) {
      it->second.image->release();
      it->second = Stored{frame, img};
    }
  }
  std::fprintf(stderr, "thumbnails: %zu kept pictures for this project\n", stored_.size());
}

void ThumbnailManager::saveStore(rn_session* s) {
  if (!s || !storeDirty_ || storeFile_.empty() || storeFile_ != storeFileFor(storeDir, s)) return;
  storeDirty_ = false;
  // The take's pictures on the finest grid (every zoom level's are among them) with their hashes.
  std::vector<uint64_t> frames(rnf_thumb_targets(rn_take_length(s), rnf_thumb_tile_step(0, 0, 0), nullptr, 0));
  rnf_thumb_targets(rn_take_length(s), rnf_thumb_tile_step(0, 0, 0), frames.data(), frames.size());
  std::vector<uint64_t> hashes(frames.size());
  if (!frames.empty() && rn_take_input_hashes(s, frames.data(), frames.size(), hashes.data()) != RN_OK) return;
  std::vector<uint64_t> current;
  for (size_t i = 0; i < frames.size(); ++i) {
    auto* img = static_cast<ThumbImage*>(rnf_thumb_cache_image_at(cache_, frames[i]));  // retained
    if (!img) continue;
    current.push_back(hashes[i]);
    auto [it, fresh] = stored_.insert({hashes[i], Stored{frames[i], img}});
    if (!fresh) {
      it->second.image->release();
      it->second = Stored{frames[i], img};
    }
  }
  // Other takes' pictures stay (switching back shows them) while the file stays small.
  constexpr size_t kMaxKept = 4000;
  std::sort(current.begin(), current.end());
  for (auto it = stored_.begin(); stored_.size() > kMaxKept && it != stored_.end();) {
    if (std::binary_search(current.begin(), current.end(), it->first)) {
      ++it;
    } else {
      it->second.image->release();
      it = stored_.erase(it);
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(storeDir, ec);
  const std::string tmp = storeFile_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(kStoreMagic, 8);
    writeValue(out, uint32_t(stored_.size()));
    std::vector<uint8_t> qoi;
    for (const auto& [hash, st] : stored_) {
      qoi.clear();
      encodeQoi(reinterpret_cast<const uint8_t*>(st.image->px.data()), st.image->width, st.image->height, &qoi);
      writeValue(out, hash);
      writeValue(out, st.frame);
      writeValue(out, uint32_t(qoi.size()));
      out.write(reinterpret_cast<const char*>(qoi.data()), std::streamsize(qoi.size()));
    }
    if (!out) {
      std::fprintf(stderr, "thumbnails: cannot write %s\n", tmp.c_str());
      return;
    }
  }
  std::filesystem::rename(tmp, storeFile_, ec);
  if (ec) std::fprintf(stderr, "thumbnails: cannot write %s: %s\n", storeFile_.c_str(), ec.message().c_str());
}

void ThumbnailManager::syncTake(rn_session* s) {
  uint64_t t = rn_active_take(s), old = rnf_thumb_cache_take(cache_);
  if (t == old) return;
  std::vector<rn_take_info> takes(rn_take_count(s));
  for (size_t i = 0; i < takes.size(); ++i) rn_take_get(s, i, &takes[i]);
  rnf_thumb_cache_rebase(cache_, t, rnf_take_shared_prefix(takes.data(), takes.size(), old, t));
}

void ThumbnailManager::takeCut(rn_session* s, uint64_t keepThrough) {
  // The same take id with new frames after keepThrough: drop the later pictures (a rebase through
  // "no take" and back keeps exactly the frames up to the cut).
  if (!s) return;
  uint64_t t = rn_active_take(s);
  if (rnf_thumb_cache_take(cache_) != t) return;  // syncTake rebases on the next picture anyway
  rnf_thumb_cache_rebase(cache_, 0, keepThrough);
  rnf_thumb_cache_rebase(cache_, t, UINT64_MAX);  // a running batch is of the old generation: dropped
  nextReconcile_ = 0;
}

void ThumbnailManager::frameShown(rn_session* s, uint64_t frame) {
  if (!enabled || !s) return;
  syncTake(s);
  uint64_t take = rn_active_take(s);
  // Before the seek bar was ever laid out too, at the finest grid: what was played never has to be
  // rendered again (the first seek bar of a session showed nothing until a whole batch was done).
  if (!rnf_thumb_cache_wants_live(cache_, frame, take)) return;
  rn_video_frame_info f{};
  rn_media_info m{};
  if (rn_session_video_frame(s, &f) != RN_OK || rn_session_media_info(s, &m) != RN_OK) return;
  ThumbImage* img = ThumbImage::make(f, m, rn_session_core_kind(s) != RN_CORE_EXTERNAL);
  if (!img) return;
  if (rnf_thumb_cache_insert(cache_, frame, take, 0, 0, img)) storeDirty_ = true;
  img->release();
}

double ThumbnailManager::layout(double width, double tileWidth, uint64_t takeLength, double now) {
  double q = rnf_thumb_tile_step(width, tileWidth, takeLength);
  if (q != step_) {
    if (step_ != 0 && takeLength > 0 && (q == step_ * 2 || q * 2 == step_)) {
      fadeFrom_ = step_;
      fadeStart_ = now;
    } else {
      fadeFrom_ = 0;
    }
    step_ = q;
  }
  rnf_thumb_cache_set_step(cache_, q);
  if (!enabled) return q;
  targetLength_ = takeLength;
  hasTarget_ = true;
  if (nextReconcile_ == 0) nextReconcile_ = now + 0.3;  // throttled (the take grows every frame)
  return q;
}

double ThumbnailManager::fadeFrom(double now) const {
  return fadeFrom_ != 0 && now - fadeStart_ < kFadeDuration ? fadeFrom_ : 0;
}
double ThumbnailManager::fadeProgress(double now) const {
  return std::min(1.0, std::max(0.0, (now - fadeStart_) / kFadeDuration));
}

void ThumbnailManager::reconcile(rn_session* s, double now) {
  if (!hasTarget_ || step_ <= 0) return;
  if (now < retryAfter_) {
    nextReconcile_ = now + 1;
    return;
  }
  uint64_t gen = rnf_thumb_cache_generation(cache_);
  if (job_ && !job_->cancelled.load()) {
    // A running batch of this generation is short: let it finish; only a take change / reset
    // makes it stale.
    if (job_->generation == gen) return;
    job_->cancelled = true;
    job_.reset();
  }
  size_t n = rnf_thumb_targets(targetLength_, step_, nullptr, 0);
  std::vector<uint64_t> targets(n);
  rnf_thumb_targets(targetLength_, step_, targets.data(), n);
  std::vector<uint64_t> missing(n);
  size_t m = rnf_thumb_cache_missing(cache_, targets.data(), n, missing.data(), n);
  missing.resize(std::min(m, n));
  // Kept pictures first (a reopened project): the ones whose take input up to their frame is the same.
  if (!missing.empty() && !stored_.empty() && s) {
    std::vector<uint64_t> hashes(missing.size());
    if (rn_take_input_hashes(s, missing.data(), missing.size(), hashes.data()) == RN_OK) {
      const uint64_t take = rnf_thumb_cache_take(cache_);
      size_t restored = 0;
      std::vector<uint64_t> still;
      for (size_t i = 0; i < missing.size(); ++i) {
        auto it = stored_.find(hashes[i]);
        if (it != stored_.end() && it->second.frame == missing[i] &&
            rnf_thumb_cache_insert(cache_, missing[i], take, 1, gen, it->second.image)) {
          ++restored;
        } else {
          still.push_back(missing[i]);
        }
      }
      if (restored) std::fprintf(stderr, "thumbnails: %zu pictures from the kept ones\n", restored);
      missing.swap(still);
    }
  }
  if (missing.empty()) return;
  auto j = std::make_shared<Job>();
  j->targets = std::move(missing);
  j->take = rnf_thumb_cache_take(cache_);
  j->generation = gen;
  j->started = now;
  j->pending = int(j->targets.size());
  job_ = j;
}

void ThumbnailManager::pump(rn_session* s, double now, bool coreBusy) {
  {
    std::lock_guard<std::mutex> lk(qMutex_);
    coreBusy_ = coreBusy;
  }
  qCv_.notify_all();
  if (!enabled) return;
  // Pictures rendered since the last pump: each one shown now (rejected by the cache if stale).
  std::vector<std::shared_ptr<Job>> done;
  std::vector<Ready> ready;
  {
    std::lock_guard<std::mutex> lk(qMutex_);
    done.swap(finished_);
    ready.swap(ready_);
  }
  for (const Ready& r : ready) {
    if (rnf_thumb_cache_insert(cache_, r.frame, r.take, 1, r.generation, r.image)) storeDirty_ = true;
    r.image->release();
  }
  bool batchDone = false;
  for (auto& j : done) {
    std::fprintf(stderr, "thumbnails: %zu pictures rendered in %.2f s%s\n", j->targets.size(), now - j->started,
                 j->cancelled.load() ? " (cancelled)" : j->failed.load() ? " (some failed)" : "");
    batchDone = batchDone || !j->cancelled.load();
    if (j->failed.load() && !j->cancelled.load()) retryAfter_ = now + 2;
    if (job_ == j) job_.reset();
    nextReconcile_ = now + 0.3;
  }
  if (!s) return;
  syncTake(s);
  if (batchDone) saveStore(s);  // what took long to render is kept at once
  if (nextReconcile_ != 0 && now >= nextReconcile_) {
    nextReconcile_ = 0;
    reconcile(s, now);
  }
  std::shared_ptr<Job> j = job_;
  if (!j || j->next >= j->targets.size()) return;
  // Admit at most one shared-machine renderer (including its queued and running phases).
  // Core creation/seeking is lazy in the worker; an admitted picture may finish once started.
  if (sharesMachine_ && (coreBusy || inFlight_.load() != 0)) return;
  size_t end = std::min(j->next + (sharesMachine_ ? 1 : kRenderersPerPump), j->targets.size());
  std::vector<Task> tasks;
  for (; j->next < end; ++j->next) {
    uint64_t f = j->targets[j->next];
    rn_renderer* r = nullptr;
    if (j->cancelled.load() || rn_active_take(s) != j->take || f > rn_take_length(s) || f == 0 ||
        rn_renderer_new(s, f - 1, f, &r) != RN_OK) {
      finishOne(j, f, nullptr, !j->cancelled.load());
      continue;
    }
    ++inFlight_;
    tasks.push_back(Task{j, r, f, rn_session_core_kind(s) != RN_CORE_EXTERNAL});
  }
  if (!tasks.empty()) {
    {
      std::lock_guard<std::mutex> lk(qMutex_);
      for (Task& t : tasks) queue_.push_back(t);
    }
    qCv_.notify_all();
  }
}

ThumbRef ThumbnailManager::imageAt(uint64_t frame) {
  return ThumbRef(static_cast<ThumbImage*>(rnf_thumb_cache_image_at(cache_, frame)));
}
ThumbRef ThumbnailManager::imageBefore(uint64_t frame, uint64_t window) {
  return ThumbRef(static_cast<ThumbImage*>(rnf_thumb_cache_image_before(cache_, frame, window)));
}
uint64_t ThumbnailManager::version() { return rnf_thumb_cache_version(cache_); }

}  // namespace rnl
