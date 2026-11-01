// No MP4 export on Android yet (RNL_HAVE_MP4_EXPORT 0: the UI offers none; docs/design/ANDROID.md, M3 =
// MediaCodec). The platform layer the shared export code (export_job.cpp, export_frame.cpp) calls.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <string>
#include <vector>

#include "mp4_export.h"
#include "render/crt_export.h"

namespace rnl {

const std::vector<ExportEncoderChoice>& exportEncoderChoices() {
  static const std::vector<ExportEncoderChoice> none;
  return none;
}

std::string availableEncodersDescription() { return "none (Android: no MP4 export yet)"; }

bool exportProject(rn_renderer*, const ExportOptions&, const std::string&, const ExportProgressFn&, const ExportCancelFn&,
                   ExportResult*, std::string* error) {
  if (error) *error = "MP4 export is not available on Android yet";
  return false;
}

bool verifyExportedMp4(const std::string&, uint64_t, std::string* error) {
  if (error) *error = "MP4 export is not available on Android yet";
  return false;
}

std::unique_ptr<ExportVideoProcessor> makeCrtExportProcessor(const CrtSettings&, std::string* error) {
  if (error) *error = "the CRT export is not available on Android yet";
  return nullptr;
}

std::function<std::unique_ptr<ExportVideoProcessor>(std::string*)> crtExportProcessorFactory(const CrtSettings& settings) {
  return [settings](std::string* error) { return makeCrtExportProcessor(settings, error); };
}

}  // namespace rnl
