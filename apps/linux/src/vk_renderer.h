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

struct ImDrawData;

namespace rnl {

struct PresentDone {
  uint64_t id = 0;
  double time = 0;  // CLOCK_MONOTONIC s when vkWaitForPresentKHR returned (just after the flip)
  bool ok = false;  // false: never confirmed (timeout, swapchain recreated, hidden window)
  int result = 0;   // last VkResult of vkWaitForPresentKHR
};

struct GameRect {
  bool visible = false;
  float x = 0, y = 0, w = 0, h = 0;  // pixels, origin top-left
  int crop = 0;                      // source pixels hidden on every side
  // CRT Display: the 4:3 tube face (no 8:7), fitted to the same height (MetalView.crtViewport).
  CrtRect crt;
  double crtCrop = 0;                // tube rows hidden at the top and bottom (fraction)
};

/// Destination rectangle of the game picture in a drawable (MetalView.viewport on macOS).
GameRect computeGameRect(int width, int height, bool integerScale, bool par87, bool hideOverscan);

class VkRenderer {
 public:
  bool init(SDL_Window* window, std::string* error);
  void shutdown();
  bool initImGui();

  bool presentWait() const { return presentWait_; }
  const std::string& description() const { return description_; }
  VkExtent2D extent() const { return extent_; }
  uint32_t imageCount() const { return uint32_t(images_.size()); }

  /// Draws (newPicture: upload it first; nullptr keeps the last one) and presents. Returns the
  /// present id (0 when nothing was presented, e.g. a minimised window). `signal`: the CRT side
  /// channel of newPicture (render/post_process.h).
  uint64_t drawAndPresent(const uint32_t* newPicture, const GameRect& rect, ImDrawData* ui,
                          const FrameSignal* signal = nullptr);

  /// Display post-process (CRT on/off + parameters). Takes effect with the next draw; switching
  /// the CRT off frees its GPU resources.
  void setPostProcess(const DisplayPostProcess& pp) { post_ = pp; }
  const DisplayPostProcess& postProcess() const { return post_; }
  PostProcessStatus postProcessStatus() const { return status_; }
  /// Seconds the GPU may spend on a new picture between the commit and its target vblank at the
  /// maximum input lead (build-ahead decision; 0 = unknown, never build ahead).
  void setGpuBudget(double seconds) { gpuBudget_ = seconds; }
  /// GPU time a new picture's build adds before it can be shown (CRT on, not built ahead): the
  /// frame loop samples input that much earlier (it is part of the sample -> screen work).
  double gpuLeadExtra() const { return status_.crtShown && !crt_.pipelined() ? crt_.gpuP90() : 0; }

  /// Presents confirmed on screen since the last call, in present order. A waiter thread calls
  /// vkWaitForPresentKHR for every present id (the frame loop never blocks on it, so the input
  /// lead may exceed one refresh).
  std::vector<PresentDone> takeCompleted();
  /// Waits for the GPU to finish the last submitted frame (fallback without present wait).
  void waitLastSubmit();
  /// Seconds the last drawAndPresent spent waiting for its slot fence + the swapchain image.
  double lastAcquireWait() const { return lastAcquireWait_; }

  // ---- UI resources (vk_ui_resources.cpp) ----
  /// Once per UI frame, before thumbTexture() calls.
  void beginUIFrame() { uiFrame_ += 1; }
  /// Filmstrip thumbnails: a 128x120 BGRA picture identified by `key` in the thumbnail atlas
  /// (uploaded before this frame's render pass if new). false: no room this frame (draw a
  /// placeholder, it is uploaded on a later frame).
  bool thumbTexture(uint64_t key, const uint32_t* px128x120, uint64_t* texId, float uv[4]);
  /// Saves the next presented frame (game + UI) as PNG (tests, docs screenshots).
  void requestScreenshot(const std::string& path);

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
