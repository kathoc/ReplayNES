// "Apply CRT effect" for the MP4 export: the live CRT pipeline (CrtRenderer) on its own window-less
// Vulkan device, synchronous plan, one frame at a time in order, read back into the encoder's
// canvas (port of CRTExportRenderer in apps/macos/Sources/Core/MP4Exporter.swift). The same frame
// sequence gives the same pictures (deterministic); the renderer hash is unaffected (display only).
//   ExportOptions opt; opt.makeProcessor = crtExportProcessorFactory(settings);
// Implemented with the Vulkan CRT on Linux (apps/linux/src/render/crt_export.cpp) and the Direct3D 11
// CRT on Windows (apps/windows/src/crt_export_d3d11.cpp).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "mp4_export.h"
#include "render/crt_settings.h"

namespace rnl {

std::unique_ptr<ExportVideoProcessor> makeCrtExportProcessor(const CrtSettings& settings, std::string* error);
std::function<std::unique_ptr<ExportVideoProcessor>(std::string*)> crtExportProcessorFactory(const CrtSettings& settings);

}  // namespace rnl
