// Vulkan compute pipeline of the nesterm physical CRT ("CRT (physical model, experimental)"), a 1:1
// port of the macOS Metal pipeline (apps/macos/Sources/Core/CRT/CRTRenderer.swift, kernels in
// apps/linux/shaders/crt/*.comp, compiled to SPIR-V at build time):
//   PPU codes -> RF/IF (FFT overlap-save FIR, M3) -> receiver + AGC (M3b/M3c, AGC on the GPU)
//   -> [raster lines] -> [supply/ABL (M4c)] -> [horizontal spot (M4C-SPOT-H)] -> tube detector +
//   scatter (M1/M4a) -> [phosphor persistence (M4b)] -> linear-light output -> show (sRGB).
// Same pass order, constants, parameters/defaults and temporal state as the Metal version. All
// per-frame work is recorded into the caller's command buffer (no readback). Display-only: it never
// touches emulation state. Not thread-safe: one thread records (the tube plan is built on a
// background thread and adopted by a later encode, like the macOS buildQueue).
// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md
#pragma once

#include <vulkan/vulkan.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "render/crt_model.h"
#include "render/crt_settings.h"  // CrtSettings, CrtRect (apps/desktop/src/render)
#include "render/crt_types.h"     // CrtInput, CrtStage, crtTubeSize (shared with Direct3D 11)

namespace rnl {

/// The Vulkan objects the CRT pipeline records into (owned by the caller).
struct CrtVulkanContext {
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  VkQueue queue = VK_NULL_HANDLE;  // only used by read() / synchronous helpers (tests, export)
};

class CrtRenderer {
 public:
  using InputKind = CrtInputKind;
  using Input = CrtInput;
  using Stage = CrtStage;

  /// Device extensions the pipeline needs (VK_KHR_push_descriptor). Empty `why` = supported.
  static bool supported(VkPhysicalDevice phys, std::string* why);
  static const char* const* requiredDeviceExtensions(uint32_t* count);

  CrtRenderer() = default;
  ~CrtRenderer();
  CrtRenderer(const CrtRenderer&) = delete;
  CrtRenderer& operator=(const CrtRenderer&) = delete;

  /// `showPass` (may be VK_NULL_HANDLE): render pass the show pipeline draws in (subpass 0).
  bool init(const CrtVulkanContext& ctx, VkRenderPass showPass, std::string* error);
  void shutdown();  // waits for the device to be idle

  /// Applies settings and the wanted tube output size. `synchronous` builds the tube plan on the
  /// calling thread (export, tests); otherwise in the background, adopted by a later encode (the
  /// previous plan keeps rendering until then, like nesterm's worker).
  void configure(const CrtSettings& s, int outputWidth, int outputHeight, bool synchronous = false);
  const CrtSettings& settings() const { return settings_; }
  /// A tube plan is queued, being built, or built and not adopted yet.
  bool planPending();

  /// Records one emulated frame's compute passes. `ordinal` = machine frame ordinal (persistence
  /// history + RF noise key); a non-increasing ordinal is a discontinuity: receiver, supply and
  /// persistence state are reset. Returns false when no tube plan is ready yet (nothing recorded).
  /// Must be outside a render pass. At most kInputSlots-1 encodes may be in flight on the GPU.
  bool encode(VkCommandBuffer cmd, const Input& input, uint64_t ordinal);
  bool hasOutput() const { return hasOutput_; }
  /// Tube output size in use (0x0 before the first plan).
  int outputWidth() const;
  int outputHeight() const;
  bool usedCodes() const { return usedCodes_; }
  /// "xCount=.. yCount=.. gCount=.. radius=..,.." of the tube plan in use (diagnostics).
  std::string planInfo() const;

  /// showprog inside the caller's render pass (`showPass` given to init): dst = destination
  /// rectangle in target pixels (origin top-left), cropFraction = tube rows hidden at the top and
  /// bottom (overscan). Bilinear in linear light when the tube is not at the destination size.
  void encodeShow(VkCommandBuffer cmd, int targetWidth, int targetHeight, const CrtRect& dst, double cropFraction);
  /// showprog into a BGRA8 buffer of tw x th pixels (offline export / tests); outside dst is black.
  void encodeShowToBuffer(VkCommandBuffer cmd, VkBuffer target, int tw, int th, const CrtRect& dst, double cropFraction);

  /// GPU seconds of the most recent encode whose timestamps are available (0 = none yet).
  double lastGpuSeconds() const { return lastGpu_; }
  /// Polls finished timestamp queries (non-blocking); returns the GPU seconds of every encode that
  /// completed since the last call.
  std::vector<double> takeGpuTimes();

  // Verification hooks (tests only).
  bool useFastScatter = true;  // register-window scatter (same taps, order and weights)
  bool useFastFFT = true;      // shared-memory FFT (same butterflies)
  bool noiseEnabled = true;    // RF noise off: the CPU reference receiver has none
  bool disableSpotH = false;   // tube-only growth
  /// Per-pass GPU timing (benchmark): timestamps after every dispatch of the next encodes;
  /// profileTimes() returns (kernel, seconds) of the last profiled encode (after it completed).
  bool profile = false;
  std::vector<std::pair<std::string, double>> profileTimes();
  /// Copies a stage buffer back (waits for the queue to be idle). Floats, RGBA per pixel.
  std::vector<float> read(Stage stage);
  /// Synchronous helper: records `fn` into a one-time command buffer, submits and waits.
  bool runSync(const std::function<void(VkCommandBuffer)>& fn);

  /// Tube output size that maps 1:1 onto a destination rectangle of `dst` pixels showing the rows
  /// between the overscan crops, capped at `maxWidth` (4:3 full raster) - CRTRenderer.tubeSize.
  static void tubeSize(double dstWidth, double dstHeight, double cropFraction, int maxWidth, int* w, int* h) {
    crtTubeSize(dstWidth, dstHeight, cropFraction, maxWidth, w, h);
  }

  static constexpr int kInputSlots = 3;

 private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* map = nullptr;
    VkDeviceSize size = 0;
  };
  struct TubeKey {
    int ow = 0, oh = 0, lines = 0;
    bool growth = false;
    double ambient = 0;
    bool operator==(const TubeKey& o) const {
      return ow == o.ow && oh == o.oh && lines == o.lines && growth == o.growth && ambient == o.ambient;
    }
  };
  struct Tube {
    TubeKey key;
    int width = 0, height = 0, ow = 0, oh = 0, xCount = 1, yCount = 1, gCount = 1, levels = 0, radius[2] = {0, 0};
    double gain = 0, scatterFraction = 0;
    bool growth = false;
    bool ringCleared = false;
    Buffer xmap, vmap, colsum, kx, ky, ambient, horizontal, emission, scatterX, scatterY, ring;
  };
  struct Slot { int ring; int64_t ordinal; };

  bool createBuffer(Buffer* b, VkDeviceSize size, bool hostVisible, bool preferDeviceLocal = true);
  void destroyBuffer(Buffer* b);
  bool uploadConst(Buffer* b, const void* data, size_t bytes);
  std::unique_ptr<Tube> makeTube(const TubeKey& k);
  void destroyTube(std::unique_ptr<Tube> t);
  void retire(std::unique_ptr<Tube> t);
  void collectRetired();
  bool ensureStages();
  void buildLoop();
  std::vector<float> persistenceWeights() const;
  uint32_t findMemory(uint32_t typeBits, VkMemoryPropertyFlags props, VkMemoryPropertyFlags avoid = 0);

  void dispatch(VkCommandBuffer cmd, const char* name, std::initializer_list<const Buffer*> buffers, const void* pc,
                uint32_t pcSize, uint32_t gx, uint32_t gy = 1);
  void barrier(VkCommandBuffer cmd);

  CrtVulkanContext ctx_;
  VkPhysicalDeviceMemoryProperties memProps_{};
  PFN_vkCmdPushDescriptorSetKHR pushDescriptor_ = nullptr;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE, showSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE, showLayout_ = VK_NULL_HANDLE;
  struct Kernel { VkPipeline pipeline = VK_NULL_HANDLE; uint32_t lx = 1, ly = 1; };
  std::map<std::string, Kernel> kernels_;
  VkPipeline showPipeline_ = VK_NULL_HANDLE;
  bool sharedFFT_ = false;
  VkCommandPool syncPool_ = VK_NULL_HANDLE;
  VkQueryPool queries_ = VK_NULL_HANDLE;
  double timestampPeriod_ = 0;
  static constexpr int kQueryPairs = 8;
  int queryNext_ = 0;
  std::vector<int> queryPending_;  // pair indices written, not yet read
  double lastGpu_ = 0;
  VkQueryPool profileQueries_ = VK_NULL_HANDLE;
  std::vector<std::string> profileNames_;
  bool profiling_ = false;

  CrtSettings settings_;
  // Receiver (fixed size).
  Buffer ping_, pong_, kernelSpec_, twiddles_, volts_, basis_, carrier_, stats_, gains_, agcState_, prepared_, rxOut_;
  // Input ring (host-visible, written right before recording).
  Buffer codeBufs_[kInputSlots], phaseBufs_[kInputSlots], rgbBufs_[kInputSlots], weightBufs_[kInputSlots];
  Buffer driveIn_;
  int inputSlot_ = 0;
  // Raster / supply / spot (per line count).
  Buffer rasterOut_, supplyMeans_, supplyRow_, supplyOut_, spotOut_, supplyState_[2];
  int supplyCurrent_ = 0;
  int stageLines_ = 0;
  bool resetReceiverPending_ = true, resetSupplyPending_ = true;
  // Start of history (first frame, or a discontinuity): the next supply pass starts from the
  // steady state of its picture (supply_state prime mode) instead of the idle tube.
  bool primeSupply_ = true;
  // Tube.
  std::unique_ptr<Tube> tube_;
  std::mutex lock_;
  std::condition_variable buildCv_;
  std::thread builder_;
  bool stopBuilder_ = false;
  bool hasPending_ = false, hasInFlight_ = false;
  TubeKey pendingKey_, inFlightKey_;
  std::unique_ptr<Tube> builtTube_;
  struct Retired { std::unique_ptr<Tube> tube; uint64_t after; };
  std::vector<Retired> retired_;
  uint64_t encodes_ = 0;
  std::vector<Slot> slots_;
  int appliedPersistence_ = -1;  // -1 unknown
  bool hasLastOrdinal_ = false;
  int64_t lastOrdinal_ = 0;
  bool hasOutput_ = false;
  bool usedCodes_ = false;
  const Buffer* lastDrive_ = nullptr;
  bool ready_ = false;
};

}  // namespace rnl
