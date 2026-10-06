// Post-process hook of the Vulkan presenter (VkRenderer::setDisplayFilter): a display-only
// filter between the uploaded 256x240 game picture and the swapchain, e.g. the CRT model
// (docs/CRT_PORT.md, owned by the CRT port). The filter records its own work (compute or render
// passes) into the frame's command buffer BEFORE the main render pass and returns the image view
// the presenter samples instead of the plain picture (nearest-neighbour blit into the game
// rectangle); the UI is drawn on top in the same pass, so overlays never add a present.
// Everything runs on the frame-loop thread.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace rnl {

struct GameRect;

struct DisplayFilterContext {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t framesInFlight = 2;  // slots: per-slot resources are indexed by `slot`
};

class DisplayFilter {
 public:
  virtual ~DisplayFilter() = default;
  /// Once, after the device exists. false: the filter stays off.
  virtual bool init(const DisplayFilterContext& ctx) = 0;
  /// Before vkDestroyDevice (the device is idle).
  virtual void shutdown() = 0;
  /// Whether the filter replaces the plain picture this frame.
  virtual bool active() const = 0;
  /// Records the filter for one presented frame. `source`: the 256x240 BGRA picture
  /// (SHADER_READ_ONLY_OPTIMAL); `newPicture`: it changed since the last call; `rect`: where the
  /// picture goes in the swapchain image. Returns a view in SHADER_READ_ONLY_OPTIMAL sampled over
  /// the whole rectangle (UV 0..1, no overscan crop applied by the presenter), or VK_NULL_HANDLE to
  /// draw the plain picture.
  virtual VkImageView record(VkCommandBuffer cmd, uint32_t slot, VkImageView source, bool newPicture,
                             const GameRect& rect) = 0;
};

}  // namespace rnl
