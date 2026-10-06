// UI resources of the Vulkan presenter: the filmstrip thumbnail atlas (one 1024x1024 BGRA
// texture of 8x8 cells, LRU by UI frame; uploads are recorded in the frame's command buffer
// before the render pass, so a cell is valid in the frame that first draws it) and screenshots
// of the presented frame (PNG).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "png_writer.h"
#include "replaynes/replaynes.h"
#include "vk_renderer.h"

namespace rnl {

namespace {
constexpr VkDeviceSize kGameBytes = VkDeviceSize(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT * 4;
}

bool VkRenderer::createAtlas() {
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_B8G8R8A8_UNORM;  // thumbnails are BGRA like rn_video
  ici.extent = {uint32_t(kAtlasSize), uint32_t(kAtlasSize), 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ici, nullptr, &atlasImage_) != VK_SUCCESS) return false;
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(device_, atlasImage_, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = findMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device_, &mai, nullptr, &atlasMem_) != VK_SUCCESS) return false;
  vkBindImageMemory(device_, atlasImage_, atlasMem_, 0);
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = atlasImage_;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = ici.format;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return vkCreateImageView(device_, &vci, nullptr, &atlasView_) == VK_SUCCESS;
}

void VkRenderer::registerAtlasWithImGui() {
  if (atlasView_ && !atlasSet_) atlasSet_ = ImGui_ImplVulkan_AddTexture(atlasView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VkRenderer::destroyAtlas() {
  if (atlasView_) vkDestroyImageView(device_, atlasView_, nullptr);
  if (atlasImage_) vkDestroyImage(device_, atlasImage_, nullptr);
  if (atlasMem_) vkFreeMemory(device_, atlasMem_, nullptr);
  atlasView_ = VK_NULL_HANDLE;
  atlasImage_ = VK_NULL_HANDLE;
  atlasMem_ = VK_NULL_HANDLE;
  atlasSet_ = VK_NULL_HANDLE;  // freed with ImGui's pool
}

bool VkRenderer::thumbTexture(uint64_t key, const uint32_t* px, uint64_t* texId, float uv[4]) {
  if (!atlasSet_) return false;
  int cell = -1;
  auto it = keyCell_.find(key);
  if (it != keyCell_.end()) {
    cell = it->second;
  } else {
    if (int(pendingThumbs_.size()) >= kAtlasUploadsPerFrame) return false;
    // A free cell, else the least recently used one not drawn this frame.
    uint64_t best = ~uint64_t(0);
    for (int i = 0; i < kAtlasCells; ++i) {
      if (cellKey_[size_t(i)] == 0) {
        cell = i;
        break;
      }
      if (cellUsed_[size_t(i)] < uiFrame_ && cellUsed_[size_t(i)] < best) {
        best = cellUsed_[size_t(i)];
        cell = i;
      }
    }
    if (cell < 0) return false;
    if (cellKey_[size_t(cell)] != 0) keyCell_.erase(cellKey_[size_t(cell)]);
    cellKey_[size_t(cell)] = key;
    keyCell_[key] = cell;
    // A previous pending upload to this cell (evicted again in the same frame) is replaced.
    pendingThumbs_.erase(std::remove_if(pendingThumbs_.begin(), pendingThumbs_.end(),
                                        [&](const PendingThumb& p) { return p.cell == cell; }),
                         pendingThumbs_.end());
    pendingThumbs_.push_back(PendingThumb{cell, std::vector<uint32_t>(px, px + 128 * 120)});
  }
  cellUsed_[size_t(cell)] = uiFrame_;
  const int perRow = kAtlasSize / kAtlasCell;
  float x0 = float((cell % perRow) * kAtlasCell), y0 = float((cell / perRow) * kAtlasCell);
  uv[0] = x0 / kAtlasSize;
  uv[1] = y0 / kAtlasSize;
  uv[2] = (x0 + 128) / kAtlasSize;
  uv[3] = (y0 + 120) / kAtlasSize;
  *texId = uint64_t(atlasSet_);
  return true;
}

void VkRenderer::recordAtlasUploads(VkCommandBuffer cmd, int slot) {
  if (!atlasImage_) return;
  if (pendingThumbs_.empty() && atlasInitialized_) return;
  Slot& s = slots_[slot];
  VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toDst.srcAccessMask = atlasInitialized_ ? VK_ACCESS_SHADER_READ_BIT : 0;
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.oldLayout = atlasInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toDst.image = atlasImage_;
  toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toDst);
  if (!atlasInitialized_) {
    // Clear once so cells never show garbage.
    VkClearColorValue black{{0.f, 0.f, 0.f, 1.f}};
    vkCmdClearColorImage(cmd, atlasImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &toDst.subresourceRange);
    atlasInitialized_ = true;
  }
  const int perRow = kAtlasSize / kAtlasCell;
  std::vector<VkBufferImageCopy> copies;
  auto* base = static_cast<uint8_t*>(s.stagingPtr) + kGameBytes;
  for (size_t i = 0; i < pendingThumbs_.size(); ++i) {
    const PendingThumb& p = pendingThumbs_[i];
    std::memcpy(base + i * kThumbBytes, p.px.data(), size_t(kThumbBytes));
    VkBufferImageCopy c{};
    c.bufferOffset = kGameBytes + i * kThumbBytes;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {(p.cell % perRow) * kAtlasCell, (p.cell / perRow) * kAtlasCell, 0};
    c.imageExtent = {128, 120, 1};
    copies.push_back(c);
  }
  if (!copies.empty())
    vkCmdCopyBufferToImage(cmd, s.staging, atlasImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
                           copies.data());
  pendingThumbs_.clear();
  VkImageMemoryBarrier toRead = toDst;
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toRead);
}

// ------------------------------------------------------------------ screenshots

void VkRenderer::requestScreenshot(const std::string& path) {
  screenshotPath_ = path;
  if (!swapchainCopyable_) std::fprintf(stderr, "screenshot: the swapchain does not allow copies\n");
}

void VkRenderer::destroyScreenshotBuffer() {
  if (shotMem_) vkFreeMemory(device_, shotMem_, nullptr);
  if (shotBuffer_) vkDestroyBuffer(device_, shotBuffer_, nullptr);
  shotMem_ = VK_NULL_HANDLE;
  shotBuffer_ = VK_NULL_HANDLE;
  shotBytes_ = 0;
}

void VkRenderer::recordScreenshot(VkCommandBuffer cmd, uint32_t imageIndex) {
  screenshotRecorded_ = false;
  if (screenshotPath_.empty() || !swapchainCopyable_) return;
  VkDeviceSize bytes = VkDeviceSize(extent_.width) * extent_.height * 4;
  if (bytes != shotBytes_) {
    destroyScreenshotBuffer();
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device_, &bci, nullptr, &shotBuffer_) != VK_SUCCESS) return;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device_, shotBuffer_, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex =
        findMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &mai, nullptr, &shotMem_) != VK_SUCCESS) {
      destroyScreenshotBuffer();
      return;
    }
    vkBindBufferMemory(device_, shotBuffer_, shotMem_, 0);
    shotBytes_ = bytes;
  }
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = images_[imageIndex];
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                       0, nullptr, 1, &b);
  VkBufferImageCopy c{};
  c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.imageExtent = {extent_.width, extent_.height, 1};
  vkCmdCopyImageToBuffer(cmd, images_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shotBuffer_, 1, &c);
  b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.dstAccessMask = 0;
  b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
  shotExtent_ = extent_;
  screenshotRecorded_ = true;
}

void VkRenderer::finishScreenshot(int slot) {
  if (!screenshotRecorded_) return;
  screenshotRecorded_ = false;
  vkWaitForFences(device_, 1, &slots_[slot].fence, VK_TRUE, UINT64_MAX);
  void* p = nullptr;
  if (vkMapMemory(device_, shotMem_, 0, shotBytes_, 0, &p) != VK_SUCCESS) return;
  int w = int(shotExtent_.width), h = int(shotExtent_.height);
  std::vector<uint8_t> rgba(size_t(w) * h * 4);
  const auto* src = static_cast<const uint8_t*>(p);
  bool bgra = format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB;
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    rgba[i * 4 + 0] = src[i * 4 + (bgra ? 2 : 0)];
    rgba[i * 4 + 1] = src[i * 4 + 1];
    rgba[i * 4 + 2] = src[i * 4 + (bgra ? 0 : 2)];
    rgba[i * 4 + 3] = 255;
  }
  vkUnmapMemory(device_, shotMem_);
  if (writePNG(screenshotPath_, w, h, rgba.data())) std::fprintf(stderr, "screenshot: %s (%dx%d)\n", screenshotPath_.c_str(), w, h);
  else std::fprintf(stderr, "screenshot: cannot write %s\n", screenshotPath_.c_str());
  screenshotPath_.clear();
}

}  // namespace rnl
