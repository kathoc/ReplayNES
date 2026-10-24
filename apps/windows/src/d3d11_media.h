// CPU-side data used by the D3D11 presenter, also tested without a Windows device.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "replaynes/replaynes.h"

namespace rnl {
inline bool d3d11FrameFits(const rn_media_info& media, const rn_video_frame_info& frame) {
  return frame.pixels && frame.width && frame.height && frame.stride >= frame.width &&
      frame.width <= media.max_width && frame.height <= media.max_height;
}

inline void d3d11CopyPicture(const rn_video_frame_info& frame, void* dst, size_t rowPitch) {
  for (uint32_t y = 0; y < frame.height; ++y)
    std::memcpy(static_cast<uint8_t*>(dst) + size_t(y) * rowPitch,
                frame.pixels + size_t(y) * frame.stride, size_t(frame.width) * 4);
}

// UVs in SV_VertexID order: top-left, top-right, bottom-left, bottom-right.
// Capacity and visible frame size differ; rotation is clockwise, as in Vulkan.
inline std::array<float, 8> d3d11PictureUV(const rn_media_info& media,
                                         const rn_video_frame_info& frame, int crop) {
  const float u[2] = {float(crop) / media.max_width, float(frame.width - crop) / media.max_width};
  const float v[2] = {float(crop) / media.max_height, float(frame.height - crop) / media.max_height};
  std::array<float, 8> out{};
  for (unsigned i = 0; i < 4; ++i) {
    unsigned x = i & 1, y = i >> 1;
    switch (media.rotation & 3) {
      case 1: { unsigned oldX = x; x = y; y = 1 - oldX; break; }
      case 2: x = 1 - x; y = 1 - y; break;
      case 3: { unsigned oldX = x; x = 1 - y; y = oldX; break; }
    }
    out[2 * i] = u[x]; out[2 * i + 1] = v[y];
  }
  return out;
}

// A screenshot reads the final swapchain, including rotation, PAR, CRT and UI.
// Preserve the legacy opaque BGRA -> RGBA conversion and ignore row padding.
inline void d3d11ScreenshotRGBA(const void* pixels, size_t rowPitch, uint32_t width,
                                uint32_t height, uint8_t* rgba) {
  for (uint32_t y = 0; y < height; ++y) {
    const auto* src = static_cast<const uint8_t*>(pixels) + size_t(y) * rowPitch;
    auto* dst = rgba + size_t(y) * width * 4;
    for (uint32_t x = 0; x < width; ++x) {
      dst[4 * x] = src[4 * x + 2]; dst[4 * x + 1] = src[4 * x + 1];
      dst[4 * x + 2] = src[4 * x]; dst[4 * x + 3] = 255;
    }
  }
}
}  // namespace rnl
