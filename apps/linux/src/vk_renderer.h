// Minimal Vulkan presenter: FIFO swapchain, the 256x240 BGRA frame drawn nearest-neighbour into a
// destination rectangle (integer / FILL scale, 8:7 pixel aspect and the overscan crop are computed
// by the caller), Dear ImGui on top, and present ids + VK_KHR_present_wait so the frame loop knows
// when each picture reached the screen (docs/FRAME_PACING.md, "Linux X11 / Vulkan").
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "render/crt_display.h"
#include "render/post_process.h"
#include "renderer.h"

struct ImDrawData;

namespace rnl {

class VkRenderer final : public Renderer {
 public:
  bool init(SDL_Window* window, std::string* error) override;
  void shutdown() override;
  /// ImGui_ImplSDL3_InitForVulkan + ImGui_ImplVulkan_Init.
  bool initImGui() override;
  void newImGuiFrame() override;

  /// VK_KHR_present_wait is available (present timestamps for the vblank grid).
  bool presentTiming() const override { return presentWait_; }
  std::string description() const override { return description_; }
  int width() const override { return int(extent_.width); }
  int height() const override { return int(extent_.height); }
  uint32_t imageCount() const { return uint32_t(images_.size()); }

  uint64_t drawAndPresent(const uint32_t* newPicture, const GameRect& rect, ImDrawData* ui,
                          const FrameSignal* signal = nullptr) override;

  /// Display post-process (CRT on/off + parameters). Takes effect with the next draw; switching
  /// the CRT off frees its GPU resources.
  void setPostProcess(const DisplayPostProcess& pp) override { post_ = pp; }
  const DisplayPostProcess& postProcess() const override { return post_; }
  PostProcessStatus postProcessStatus() const override { return status_; }
  /// Seconds the GPU may spend on a new picture between the commit and its target vblank at the
  /// maximum input lead (build-ahead decision; 0 = unknown, never build ahead).
  void setGpuBudget(double seconds) override { gpuBudget_ = seconds; }
  /// GPU time a new picture's build adds before it can be shown (CRT on, not built ahead): the
  /// frame loop samples input that much earlier (it is part of the sample -> screen work).
  double gpuLeadExtra() const override { return status_.crtShown && !crt_.pipelined() ? crt_.gpuP90() : 0; }

  /// Presents confirmed on screen since the last call, in present order. A waiter thread calls
  /// vkWaitForPresentKHR for every present id (the frame loop never blocks on it, so the input
  /// lead may exceed one refresh).
  std::vector<PresentDone> takeCompleted() override;
  /// Waits for the GPU to finish the last submitted frame (fallback without present wait).
  void waitLastSubmit();
  /// Seconds the last drawAndPresent spent waiting for its slot fence + the swapchain image.
  double lastAcquireWait() const override { return lastAcquireWait_; }

  // ---- UI resources (vk_ui_resources.cpp) ----
  void beginUIFrame() override { uiFrame_ += 1; }
  bool thumbTexture(uint64_t key, const uint32_t* px128x120, uint64_t* texId, float uv[4]) override;
  void requestScreenshot(const std::string& path) override;

 private:
  void waiterLoop();
  bool createDevice(std::string* error);
  bool createSwapchain();
  void destroySwapchain();
  bool createPipeline();
  bool createGameTexture();
  uint32_t findMemory(uint32_t typeBits, VkMemoryPropertyFlags props);
  // vk_ui_resources.cpp
  bool createAtlas();
  void destroyAtlas();
  void registerAtlasWithImGui();
  void recordAtlasUploads(VkCommandBuffer cmd, int slot);
  void recordScreenshot(VkCommandBuffer cmd, uint32_t imageIndex);
  void finishScreenshot(int slot);
  void destroyScreenshotBuffer();

  SDL_Window* window_ = nullptr;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t apiVersion_ = VK_API_VERSION_1_1;
  bool presentWait_ = false;
  std::string description_;

  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
  VkExtent2D extent_{0, 0};
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  std::vector<VkFramebuffer> framebuffers_;
  std::vector<VkSemaphore> renderDone_;  // per swapchain image
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  bool needRecreate_ = false;
  uint64_t presentId_ = 0;
  // Present-wait thread: ids to wait for (id, swapchain generation) and the results.
  std::thread waiter_;
  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::deque<std::pair<uint64_t, uint64_t>> toWait_;
  std::vector<PresentDone> done_;
  std::atomic<bool> stopWaiter_{false};
  std::mutex swapMutex_;  // swapchain_ replacement vs. the waiter's vkWaitForPresentKHR
  uint64_t swapGen_ = 0;

  static constexpr int kSlots = 2;
  static constexpr int kAtlasCell = 128, kAtlasSize = 1024, kAtlasCells = (kAtlasSize / kAtlasCell) * (kAtlasSize / kAtlasCell);
  static constexpr int kAtlasUploadsPerFrame = 12;
  static constexpr VkDeviceSize kThumbBytes = VkDeviceSize(128) * 120 * 4;
  struct Slot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* stagingPtr = nullptr;
  };
  Slot slots_[kSlots];
  VkCommandBuffer buildCmds_[kSlots] = {};  // CRT build-ahead: recorded after the present's commands
  CrtDisplay crt_;
  DisplayPostProcess post_;
  PostProcessStatus status_;
  bool crtSupported_ = false;
  double gpuBudget_ = 0;
  DisplayPostProcess shownPost_;
  CrtRect shownCrt_;
  int slot_ = 0;
  VkCommandPool pool_ = VK_NULL_HANDLE;

  VkImage gameImage_ = VK_NULL_HANDLE;
  VkDeviceMemory gameMem_ = VK_NULL_HANDLE;
  VkImageView gameView_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  bool hasPicture_ = false;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool descPool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  bool imguiReady_ = false;
  double lastAcquireWait_ = 0;
  // Thumbnail atlas.
  VkImage atlasImage_ = VK_NULL_HANDLE;
  VkDeviceMemory atlasMem_ = VK_NULL_HANDLE;
  VkImageView atlasView_ = VK_NULL_HANDLE;
  VkDescriptorSet atlasSet_ = VK_NULL_HANDLE;  // ImGui texture id
  bool atlasInitialized_ = false;               // layout transitioned from UNDEFINED
  uint64_t uiFrame_ = 0;
  std::array<uint64_t, kAtlasCells> cellKey_{};
  std::array<uint64_t, kAtlasCells> cellUsed_{};
  std::unordered_map<uint64_t, int> keyCell_;
  struct PendingThumb {
    int cell;
    std::vector<uint32_t> px;
  };
  std::vector<PendingThumb> pendingThumbs_;
  // Screenshot.
  std::string screenshotPath_;
  bool screenshotRecorded_ = false;
  bool swapchainCopyable_ = false;
  VkBuffer shotBuffer_ = VK_NULL_HANDLE;
  VkDeviceMemory shotMem_ = VK_NULL_HANDLE;
  VkDeviceSize shotBytes_ = 0;
  VkExtent2D shotExtent_{0, 0};
};

}  // namespace rnl
