// ExportJob (export/mp4_export.h): the UI's worker thread around exportProject(), shared by the
// Linux (FFmpeg) and Windows (Media Foundation) exporters.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>

#include "mp4_export.h"

namespace rnl {

// ------------------------------------------------------------------ ExportJob
ExportJob::~ExportJob() {
  cancel_ = true;
  if (worker_.joinable()) worker_.join();
}

bool ExportJob::start(rn_session* session, ExportOptions opt, std::string outPath) {
  if (running_) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = "An export is already running";
    return false;
  }
  if (worker_.joinable()) worker_.join();
  cancel_ = false;
  finished_ = false;
  ok_ = false;
  done_ = 0;
  total_ = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result_ = ExportResult{};
    error_.clear();
  }
  outPath_ = outPath;
  rn_renderer* r = nullptr;
  rn_status st = session ? rn_renderer_new(session, opt.settings.start_frame, opt.settings.end_frame, &r)
                         : RN_ERR_INVALID_ARG;
  if (st != RN_OK || !r) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::string(rn_status_name(st)) + ": " + (session ? rn_last_error() : "no session");
    finished_ = true;
    return false;
  }
  total_ = rn_renderer_total_frames(r);
  running_ = true;
  worker_ = std::thread([this, r, opt = std::move(opt), outPath = std::move(outPath)]() {
    ExportResult res;
    std::string err;
    const bool ok = exportProject(
        r, opt, outPath,
        [this](uint64_t d, uint64_t t) {
          done_ = d;
          total_ = t;
        },
        [this]() { return cancel_.load(); }, &res, &err);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      result_ = res;
      error_ = ok ? std::string() : err;
    }
    ok_ = ok;
    finished_ = true;  // before running_ = false: never "neither running nor finished"
    running_ = false;
  });
  return true;
}

void ExportJob::cancel() { cancel_ = true; }
bool ExportJob::running() const { return running_.load(); }
bool ExportJob::finished() const { return finished_.load(); }
bool ExportJob::succeeded() const { return finished_.load() && ok_.load(); }
bool ExportJob::wasCancelled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return finished_.load() && error_ == "cancelled";
}
double ExportJob::progress() const {
  const uint64_t t = total_.load();
  return t ? std::min(1.0, double(done_.load()) / double(t)) : 0.0;
}
ExportResult ExportJob::result() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return result_;
}
std::string ExportJob::error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}
void ExportJob::wait() {
  if (worker_.joinable()) worker_.join();
}

}  // namespace rnl
