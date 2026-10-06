// "Apply CRT effect" for the MP4 export: the live CRT pipeline (CrtRenderer) on its own window-less
// Vulkan device, synchronous plan, one frame at a time in order, read back into the encoder's
// canvas (port of CRTExportRenderer in apps/macos/Sources/Core/MP4Exporter.swift). The same frame
// sequence gives the same pictures (deterministic); the renderer hash is unaffected (display only).
//   ExportOptions opt; opt.makeProcessor = crtExportProcessorFactory(settings);
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "mp4_export.h"
#include "render/crt_renderer.h"

namespace rnl {

std::unique_ptr<ExportVideoProcessor> makeCrtExportProcessor(const CrtSettings& settings, std::string* error);
std::function<std::unique_ptr<ExportVideoProcessor>(std::string*)> crtExportProcessorFactory(const CrtSettings& settings);

}  // namespace rnl
