// Windows: MP4 export and the CRT export processor are not ported yet (step 2: Media Foundation
// H.264 + AAC, the CRT in HLSL; docs/WINDOWS.md). The shared UI hides Export… (RNL_HAVE_MP4_EXPORT=0);
// these keep the seams (export/mp4_export.h, render/crt_export.h) linkable and report why.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "mp4_export.h"
#include "render/crt_export.h"

namespace rnl {

namespace {
const char kUnavailable[] = "MP4 export is not available in the Windows version yet";
}

rnf_export_settings defaultExportSettings() {
  rnf_export_settings s{};
  s.preset.kind = RNF_PRESET_CANVAS;
  s.preset.width = 1280;
  s.preset.height = 960;
  s.crop_top = 8;
  s.crop_bottom = 8;
  return s;
}

bool exportProject(rn_renderer* renderer, const ExportOptions&, const std::string&, const ExportProgressFn&,
                   const ExportCancelFn&, ExportResult*, std::string* error) {
  rn_renderer_free(renderer);
  if (error) *error = kUnavailable;
  return false;
}

std::string availableEncodersDescription() { return "none (Windows: not ported yet)"; }

ExportJob::~ExportJob() = default;

bool ExportJob::start(rn_session*, ExportOptions, std::string outPath) {
  outPath_ = std::move(outPath);
  std::lock_guard<std::mutex> lk(mutex_);
  error_ = kUnavailable;
  finished_ = true;
  ok_ = false;
  return false;
}
void ExportJob::cancel() { cancel_ = true; }
bool ExportJob::running() const { return running_.load(); }
bool ExportJob::finished() const { return finished_.load(); }
bool ExportJob::succeeded() const { return false; }
bool ExportJob::wasCancelled() const { return false; }
double ExportJob::progress() const { return 0; }
ExportResult ExportJob::result() const { return {}; }
std::string ExportJob::error() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return error_;
}
void ExportJob::wait() {}

std::unique_ptr<ExportVideoProcessor> makeCrtExportProcessor(const CrtSettings&, std::string* error) {
  if (error) *error = "the CRT display is not available in the Windows version yet";
  return nullptr;
}

std::function<std::unique_ptr<ExportVideoProcessor>(std::string*)> crtExportProcessorFactory(const CrtSettings&) {
  return {};
}

}  // namespace rnl
