// Filmstrip thumbnails of the active take (Filmstrip.swift + TimelineActions.swift on macOS):
//  * the cache and its policy are the shared core's rnf_thumb_cache (fixed grid of 5 s * 2^k,
//    shared-prefix rebase on take switches); payloads are ThumbImage (128 pixels wide, display aspect, refcounted);
//  * live capture: a picture the emulation shows anyway is kept when the grid wants it;
//  * background batches: the missing pictures are rendered from the take's checkpoints with one
//    rn_renderer per picture (created on the frame thread, a few per frame), run on low-priority
//    worker threads, and committed to the cache in one go (a reopened project shows its whole
//    filmstrip at once).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "emulation.h"
#include "replaynes/frontend.h"

namespace rnl {

bool thumbnailSize(const rn_video_frame_info& frame, const rn_media_info& media, uint32_t* width, uint32_t* height, bool nesPicture = false);

struct ThumbImage {
  std::atomic<int> refs{1};
  uint64_t serial = 0;  // unique per image (texture atlas key)
  uint32_t width = RNF_THUMB_WIDTH, height = RNF_THUMB_HEIGHT;
  std::vector<uint32_t> px;
  static ThumbImage* make(const uint32_t* frame256x240);
  static ThumbImage* make(const rn_video_frame_info& frame, const rn_media_info& media, bool nesPicture = false);
  /// A copy of a thumbnail-sized picture (RNF_THUMB_WIDTH x RNF_THUMB_HEIGHT), new serial.
  static ThumbImage* fromThumb(const uint32_t* px, uint32_t width = RNF_THUMB_WIDTH, uint32_t height = RNF_THUMB_HEIGHT);
  void retain() { refs.fetch_add(1); }
  void release() {
    if (refs.fetch_sub(1) == 1) delete this;
  }
};

/// RAII holder of a retained ThumbImage (cache lookups return retained images).
class ThumbRef {
 public:
  ThumbRef() = default;
  explicit ThumbRef(ThumbImage* p) : p_(p) {}
  ThumbRef(const ThumbRef&) = delete;
  ThumbRef& operator=(const ThumbRef&) = delete;
  ThumbRef(ThumbRef&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  ThumbRef& operator=(ThumbRef&& o) noexcept {
    if (this != &o) {
      if (p_) p_->release();
      p_ = o.p_;
      o.p_ = nullptr;
    }
    return *this;
  }
  ~ThumbRef() {
    if (p_) p_->release();
  }
  ThumbImage* get() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  ThumbImage* p_ = nullptr;
};

class ThumbnailManager : public FrameObserver {
  friend struct ThumbnailSchedulingProbe;
 public:
  ThumbnailManager();
  ~ThumbnailManager() override;

  /// -no-thumbnails / perf runs: nothing is captured or rendered.
  bool enabled = true;

  void sessionInstalled(rn_session* s) override;
  void frameShown(rn_session* s, uint64_t frame) override;
  void takeCut(rn_session* s, uint64_t keepThrough) override;

  /// The timeline was laid out (UI): picks the tile step (= thumbnail grid) and schedules the
  /// missing pictures. Returns the step.
  double layout(double width, double tileWidth, uint64_t takeLength, double now);
  /// Previous step while the scale changes (crossfade); 0 when none.
  double fadeFrom(double now) const;
  double fadeProgress(double now) const;  // 0...1
  double step() const { return step_; }

  /// Frame thread, every loop iteration: keeps the cache on the active take, creates the
  /// renderers of a running batch (a few per call) and commits finished batches.
  void pump(rn_session* s, double now, bool coreBusy = false);

  ThumbRef imageAt(uint64_t frame);
  ThumbRef imageBefore(uint64_t frame, uint64_t window);
  uint64_t version();

 private:
  struct Job {
    std::vector<uint64_t> targets;
    uint64_t take = 0, generation = 0;
    size_t next = 0;
    std::atomic<int> pending{0};
    std::atomic<bool> cancelled{false}, failed{false};
    std::mutex m;
    std::vector<std::pair<uint64_t, ThumbImage*>> results;
    ~Job() {
      // A cancelled, partly admitted batch never reaches pending == 0.
      for (auto& result : results) result.second->release();
    }
  };
  struct Task {
    std::shared_ptr<Job> job;
    rn_renderer* renderer;
    uint64_t frame;
    bool nesPicture;
  };
  void syncTake(rn_session* s);
  void reconcile(double now);
  void finishOne(const std::shared_ptr<Job>& j, uint64_t frame, ThumbImage* img, bool failed);
  void workerLoop();
  void resizeWorkers(unsigned count);

  rnf_thumb_cache* cache_ = nullptr;
  double step_ = 0;
  double fadeFrom_ = 0, fadeStart_ = 0;
  uint64_t targetLength_ = 0;
  bool hasTarget_ = false;
  double nextReconcile_ = 0, retryAfter_ = 0;
  std::shared_ptr<Job> job_;
  std::vector<std::shared_ptr<Job>> finished_;  // guarded by qMutex_
  std::vector<std::thread> workers_;
  std::mutex qMutex_;
  std::condition_variable qCv_;
  std::deque<Task> queue_;
  bool stop_ = false;
  bool sharesMachine_ = false;
  bool coreBusy_ = false;
  std::atomic<unsigned> inFlight_{0};
};

}  // namespace rnl
