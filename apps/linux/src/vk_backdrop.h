// The blurred backdrop on Vulkan (render/backdrop.h): a copy of each new picture with its mip chain
// (vkCmdBlitImage, linear), drawn under the picture through backdrop.frag.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>

#include "render/backdrop.h"

namespace rnl {

class VkBackdrop {
 public:
  using FindMemory = std::function<uint32_t(uint32_t, VkMemoryPropertyFlags)>;
  /// The pipeline for `renderPass` (vertex shader: the picture's blit.vert).
  bool create(VkDevice device, VkRenderPass renderPass, const uint32_t* vert, size_t vertBytes, FindMemory findMemory);
  void release();
  /// Outside a render pass: copies the region w x h of `picture` (SHADER_READ_ONLY, TRANSFER_SRC usage;
  /// left as it was) and builds the mip chain. A new size waits for the device (a new texture).
  void update(VkCommandBuffer cmd, VkImage picture, uint32_t w, uint32_t h);
  bool ready() const { return valid_; }
  int width() const { return int(w_); }
  int height() const { return int(h_); }
  /// Inside the render pass: the plan's rectangle (scissored to the screen). `uv`: the source rectangle
  /// in the copy (0..1) and the rotation, as blit.vert takes them.
  void draw(VkCommandBuffer cmd, const BackdropPlan& plan, VkExtent2D screen, const float uv[8]);

 private:
  void destroyImage();
  VkDevice device_ = VK_NULL_HANDLE;
  FindMemory findMemory_;
  VkImage image_ = VK_NULL_HANDLE;
  VkDeviceMemory mem_ = VK_NULL_HANDLE;
  VkImageView view_ = VK_NULL_HANDLE;
  uint32_t w_ = 0, h_ = 0, levels_ = 0;
  bool valid_ = false;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rnl
