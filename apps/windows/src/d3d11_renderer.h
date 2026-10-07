// Direct3D 11 presenter of the Windows frontend (Renderer, apps/desktop/src/renderer.h):
//   * device: hardware (feature level 11_1 ... 10_0), WARP as the fallback
//   * flip-model swap chain (DXGI_SWAP_EFFECT_FLIP_DISCARD, 3 buffers, B8G8R8A8), frame latency
//     waitable object with SetMaximumFrameLatency(1), DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING when the
//     system supports it (needed for variable refresh / tearing presents)
//   * the 256x240 picture in a dynamic texture, drawn nearest-neighbour into the GameRect
//     (integer / FILL / 8:7 / overscan crop), Dear ImGui (imgui_impl_dx11) on top
//   * pacing source: a waiter thread reads IDXGISwapChain::GetFrameStatistics whenever the frame
//     latency waitable object is signalled (a frame left the queue) and every 2 ms while presents
//     are pending: a present is on screen when the statistics' PresentCount reaches its
//     GetLastPresentCount, at SyncQPCTime (the vblank, on the QPC clock = nowSeconds()). Presents
//     the statistics skip are not confirmed. Without statistics (some drivers, remote sessions)
//     the waitable object's signal time stands in (like vkWaitForPresentKHR returning on Linux).
//     The frame loop's DisplayScheduler builds the vblank grid from these.
//   * --vrr in full screen: Present(0, DXGI_PRESENT_ALLOW_TEARING) and no present timing (the loop
//     paces on the host clock at the NES rate; a variable refresh display follows it)
// The CRT display is not ported yet (crtAvailable = false; docs/WINDOWS.md).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "renderer.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11SamplerState;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11Buffer;
struct ID3D11RasterizerState;
struct ID3D11BlendState;
struct IDXGISwapChain2;
struct IDXGIFactory2;

namespace rnl {

class D3D11Renderer final : public Renderer {
 public:
  explicit D3D11Renderer(bool vrr) : vrr_(vrr) {}
  ~D3D11Renderer() override;

  bool init(SDL_Window* window, std::string* error) override;
  bool initImGui() override;
  void shutdown() override;
  void newImGuiFrame() override;

  bool presentTiming() const override;
  std::string description() const override { return description_; }
  int width() const override { return width_; }
  int height() const override { return height_; }

  uint64_t drawAndPresent(const uint32_t* newPicture, const GameRect& rect, ImDrawData* ui,
                          const FrameSignal* signal = nullptr) override;
  std::vector<PresentDone> takeCompleted() override;
  double lastAcquireWait() const override { return lastAcquireWait_; }
  /// From the frame statistics: (SyncQPCTime difference) / (SyncRefreshCount difference) over
  /// 1-3 s of vblanks (exact refresh counts; the timestamps' jitter averages out).
  double refreshPeriod() const override { return refreshPeriod_.load(); }

  void setPostProcess(const DisplayPostProcess& pp) override { post_ = pp; }
  const DisplayPostProcess& postProcess() const override { return post_; }
  PostProcessStatus postProcessStatus() const override { return status_; }

  void beginUIFrame() override { uiFrame_ += 1; }
  bool thumbTexture(uint64_t key, const uint32_t* px128x120, uint64_t* texId, float uv[4]) override;
  void requestScreenshot(const std::string& path) override { screenshotPath_ = path; }

 private:
  bool createDevice(std::string* error);
  bool createSwapChain(std::string* error);
  bool createTargets();
  void releaseTargets();
  bool resize(int w, int h);
  bool createPipeline(std::string* error);
  bool createTextures();
  void uploadThumbs();
  void saveScreenshot();
  bool fullscreenNow() const;
  void waiterLoop();

  SDL_Window* window_ = nullptr;
  void* hwnd_ = nullptr;
  const bool vrr_;
  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* ctx_ = nullptr;
  IDXGIFactory2* factory_ = nullptr;
  IDXGISwapChain2* swap_ = nullptr;
  void* waitable_ = nullptr;  // HANDLE
  unsigned swapFlags_ = 0;
  bool tearingSupported_ = false;
  ID3D11RenderTargetView* rtv_ = nullptr;
  ID3D11Texture2D* backBuffer_ = nullptr;
  int width_ = 0, height_ = 0;
  std::string description_;
  bool imguiReady_ = false;

  ID3D11Texture2D* gameTex_ = nullptr;
  ID3D11ShaderResourceView* gameSrv_ = nullptr;
  bool hasPicture_ = false;
  ID3D11SamplerState* point_ = nullptr;
  ID3D11VertexShader* vs_ = nullptr;
  ID3D11PixelShader* ps_ = nullptr;
  ID3D11Buffer* cb_ = nullptr;
  ID3D11RasterizerState* raster_ = nullptr;
  ID3D11BlendState* opaque_ = nullptr;

  // Thumbnail atlas (1024x1024 BGRA, 8x8 cells of 128x120, LRU by UI frame).
  static constexpr int kAtlasCell = 128, kAtlasSize = 1024, kAtlasCells = (kAtlasSize / kAtlasCell) * (kAtlasSize / kAtlasCell);
  static constexpr int kAtlasUploadsPerFrame = 12;
  ID3D11Texture2D* atlas_ = nullptr;
  ID3D11ShaderResourceView* atlasSrv_ = nullptr;
  uint64_t uiFrame_ = 0;
  std::array<uint64_t, kAtlasCells> cellKey_{};
  std::array<uint64_t, kAtlasCells> cellUsed_{};
  std::unordered_map<uint64_t, int> keyCell_;
  struct PendingThumb {
    int cell;
    std::vector<uint32_t> px;
  };
  std::vector<PendingThumb> pendingThumbs_;

  DisplayPostProcess post_;
  PostProcessStatus status_;
  double lastAcquireWait_ = 0;
  std::string screenshotPath_;

  // Present ids and the waiter thread.
  uint64_t presentId_ = 0;
  uint64_t generation_ = 0;  // bumped by a resize: waits queued before it are not confirmed
  struct Pending {
    uint64_t id, presentCount, generation;
  };
  std::thread waiter_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Pending> toWait_;
  std::vector<PresentDone> done_;
  bool stop_ = false;
  std::mutex swapMutex_;  // GetFrameStatistics (waiter) vs. ResizeBuffers / Present
  std::atomic<uint64_t> currentGeneration_{0};
  std::atomic<double> refreshPeriod_{0};
};

}  // namespace rnl
