// A window-less Vulkan device for compute work off the display path: the CRT conformance test /
// benchmark and the offline (export) CRT renderer. Picks a device with a graphics+compute queue
// and VK_KHR_push_descriptor (REPLAYNES_VK_DEVICE=<substring> selects one by name, e.g. "llvmpipe"
// for Mesa's lavapipe). On macOS (MoltenVK, development only) it enables portability enumeration
// and turns MoltenVK's fast math off (IEEE float like the Metal port's safe math mode).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <vulkan/vulkan.h>

#include <string>

#include "render/crt_renderer.h"

namespace rnl {

class HeadlessVulkan {
 public:
  ~HeadlessVulkan() { shutdown(); }
  bool init(std::string* error);
  void shutdown();
  CrtVulkanContext context() const { return CrtVulkanContext{phys_, device_, queueFamily_, queue_}; }
  const std::string& description() const { return description_; }

 private:
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  std::string description_;
};

}  // namespace rnl
