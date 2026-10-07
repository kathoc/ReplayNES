// Display post-process interface of the desktop frontend (the hook the UI uses): CRT on/off +
// parameters, and what the renderer reports back. The CRT is display-only: it never touches
// emulation state, recorded input or hashes.
//   Renderer::setPostProcess(DisplayPostProcess)   (renderer.h; Vulkan: apps/linux, D3D11: apps/windows)
//   Renderer::drawAndPresent(picture, rect, ui, &signal)
//   Renderer::postProcessStatus()
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include "render/crt_settings.h"

namespace rnl {

/// What the display shows on top of the emulated picture.
struct DisplayPostProcess {
  bool crt = false;                // "CRT Display" (off by default, the crisp picture)
  CrtSettings crtSettings;         // nesterm defaults; sanitized when applied
  int maxTubeWidth = 1600;         // nesterm OUTPUT-RESOLUTION-SPEC cap (4:3 -> 1600x1200)
  bool adaptiveResolution = true;  // lower the tube resolution when the GPU can't keep 60 fps
  bool allowBuildAhead = true;     // build GPU-heavy pictures one frame ahead when they wouldn't fit
  bool operator==(const DisplayPostProcess& o) const {
    return crt == o.crt && crtSettings == o.crtSettings && maxTubeWidth == o.maxTubeWidth &&
           adaptiveResolution == o.adaptiveResolution && allowBuildAhead == o.allowBuildAhead;
  }
  bool operator!=(const DisplayPostProcess& o) const { return !(*this == o); }
};

/// The CRT signal side channel of a new picture (EmuHost::signal(); display only).
struct FrameSignal {
  const uint16_t* codes = nullptr;  // rn_video_indices codes of the same picture (nullptr: none)
  uint32_t burstPhase = 0;
  uint64_t ordinal = 0;             // machine frame ordinal (the take frame when there are no codes)
  bool flashAltered = false;        // the flash filter changed the picture: the CRT takes its RGB
};

struct PostProcessStatus {
  bool crtAvailable = false;  // the device can run it (VK_KHR_push_descriptor)
  std::string crtError;       // why not / last failure
  bool crtShown = false;      // the last present showed the CRT picture
  int tubeWidth = 0, tubeHeight = 0;
  double scale = 1;           // adaptive resolution factor of the tube (1 = 1:1 with the screen)
  double gpuMsP50 = 0, gpuMsP90 = 0;  // GPU time of the CRT passes per new picture
  bool buildAhead = false;    // pictures are built one frame ahead
  bool usedCodes = false;     // RF path (PPU codes) vs the RGB picture
};

}  // namespace rnl
