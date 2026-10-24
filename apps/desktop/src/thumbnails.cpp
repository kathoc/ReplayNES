// SPDX-License-Identifier: GPL-2.0-or-later
#include "thumbnails.h"

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
  for (int i = 0; i < n; ++i) workers_.emplace_back([this] { workerLoop(); });
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
  for (auto& j : finished_)
    for (auto& r : j->results) r.second->release();
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
      qCv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
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
  }
}

void ThumbnailManager::finishOne(const std::shared_ptr<Job>& j, uint64_t frame, ThumbImage* img, bool failed) {
  {
    std::lock_guard<std::mutex> lk(j->m);
    if (img) j->results.push_back({frame, img});
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
  rnf_thumb_cache_reset(cache_, s ? rn_active_take(s) : 0);
  if (step_ > 0) rnf_thumb_cache_set_step(cache_, step_);
  hasTarget_ = false;
  nextReconcile_ = 0;
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
  if (!rnf_thumb_cache_wants(cache_, frame, take)) return;
  rn_video_frame_info f{};
  rn_media_info m{};
  if (rn_session_video_frame(s, &f) != RN_OK || rn_session_media_info(s, &m) != RN_OK) return;
  ThumbImage* img = ThumbImage::make(f, m, rn_session_core_kind(s) != RN_CORE_EXTERNAL);
  if (!img) return;
  rnf_thumb_cache_insert(cache_, frame, take, 0, 0, img);
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

void ThumbnailManager::reconcile(double now) {
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
  if (missing.empty()) return;
  auto j = std::make_shared<Job>();
  j->targets = std::move(missing);
  j->take = rnf_thumb_cache_take(cache_);
  j->generation = gen;
  j->pending = int(j->targets.size());
  job_ = j;
}

void ThumbnailManager::pump(rn_session* s, double now) {
  if (!enabled) return;
  // Finished batches: committed in one update (rejected by the cache if stale).
  std::vector<std::shared_ptr<Job>> done;
  {
    std::lock_guard<std::mutex> lk(qMutex_);
    done.swap(finished_);
  }
  for (auto& j : done) {
    std::vector<uint64_t> frames;
    std::vector<void*> payloads;
    for (auto& r : j->results) {
      frames.push_back(r.first);
      payloads.push_back(r.second);
    }
    if (!frames.empty())
      rnf_thumb_cache_insert_batch(cache_, frames.data(), payloads.data(), frames.size(), j->take, j->generation);
    for (auto& r : j->results) r.second->release();
    j->results.clear();
    if (j->failed.load() && !j->cancelled.load()) retryAfter_ = now + 2;
    if (job_ == j) job_.reset();
    nextReconcile_ = now + 0.3;
  }
  if (!s) return;
  syncTake(s);
  if (nextReconcile_ != 0 && now >= nextReconcile_) {
    nextReconcile_ = 0;
    reconcile(now);
  }
  std::shared_ptr<Job> j = job_;
  if (!j || j->next >= j->targets.size()) return;
  size_t end = std::min(j->next + kRenderersPerPump, j->targets.size());
  std::vector<Task> tasks;
  for (; j->next < end; ++j->next) {
    uint64_t f = j->targets[j->next];
    rn_renderer* r = nullptr;
    if (j->cancelled.load() || rn_active_take(s) != j->take || f > rn_take_length(s) || f == 0 ||
        rn_renderer_new(s, f - 1, f, &r) != RN_OK) {
      finishOne(j, f, nullptr, !j->cancelled.load());
      continue;
    }
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
