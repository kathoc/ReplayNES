// The swapchain's size and transform from what the surface reports (Vulkan; docs/design/ANDROID.md).
// Android devices differ: a portrait panel used in landscape (the Retroid Pocket 5, turned 90 degrees),
// a landscape panel, a phone turned either way, a window smaller than the display (bars, cutouts), and
// drivers report a turned surface's extent in either orientation. Taking the surface's transform tells
// the compositor the frames come pre-rotated, which they are not (the picture showed sideways). The rule
// for every device: an identity swapchain when the surface supports it, the size of the window; the
// compositor rotates it. Plain integers: the values of VkSurfaceTransformFlagBitsKHR / VkSurfaceCapabilitiesKHR.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace rnl {

constexpr uint32_t kSurfaceIdentity = 0x1, kSurfaceRotate90 = 0x2, kSurfaceRotate180 = 0x4, kSurfaceRotate270 = 0x8;
constexpr uint32_t kSurfaceUndefinedExtent = 0xFFFFFFFFu;

struct SurfaceCaps {
  uint32_t width = kSurfaceUndefinedExtent, height = kSurfaceUndefinedExtent;  // currentExtent
  uint32_t minWidth = 1, minHeight = 1, maxWidth = 16384, maxHeight = 16384;
  uint32_t currentTransform = kSurfaceIdentity, supportedTransforms = kSurfaceIdentity;
};

struct SurfacePlan {
  uint32_t width = 0, height = 0;  // 0: minimised (no swapchain)
  uint32_t transform = kSurfaceIdentity;
  int rotation = 0;  // the surface's own quarter turn in degrees (0, 90, 180, 270), for the log
};

inline int surfaceRotation(uint32_t t) {
  return (t & kSurfaceRotate90) ? 90 : (t & kSurfaceRotate180) ? 180 : (t & kSurfaceRotate270) ? 270 : 0;
}

/// `windowW` x `windowH`: the window in pixels (SDL_GetWindowSizeInPixels). Drivers differ in which
/// orientation a turned surface reports its extent in (the Retroid Pocket 5's Adreno: 1920 x 1080 with
/// a 90 degree transform; others the panel's 1080 x 1920), so with an identity swapchain the window's
/// size decides, and the reported extent only says which way the limits are.
inline SurfacePlan planSurface(const SurfaceCaps& c, int windowW, int windowH) {
  SurfacePlan p;
  p.rotation = surfaceRotation(c.currentTransform);
  const bool identity = (c.supportedTransforms & kSurfaceIdentity) != 0;
  const bool defined = c.width != kSurfaceUndefinedExtent;
  uint32_t w, h, minW = c.minWidth, minH = c.minHeight, maxW = c.maxWidth, maxH = c.maxHeight;
  if (!identity) {
    // Its own transform and extent (pre-rotated frames would be its business).
    p.transform = c.currentTransform;
    w = defined ? c.width : uint32_t(std::max(0, windowW));
    h = defined ? c.height : uint32_t(std::max(0, windowH));
  } else {
    p.transform = kSurfaceIdentity;
    if (windowW > 0 && windowH > 0) {
      w = uint32_t(windowW);
      h = uint32_t(windowH);
    } else {
      w = defined ? c.width : 0;
      h = defined ? c.height : 0;
    }
    // Limits reported the other way round than the window: turned alike.
    if (defined && (c.width > c.height) != (w > h) && c.width != c.height && w != h) {
      std::swap(minW, minH);
      std::swap(maxW, maxH);
    }
  }
  if (w == 0 || h == 0) return p;  // minimised
  p.width = std::clamp(w, minW, std::max(minW, maxW));
  p.height = std::clamp(h, minH, std::max(minH, maxH));
  return p;
}

}  // namespace rnl
