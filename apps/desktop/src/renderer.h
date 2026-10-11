// The presenter seam of the desktop frontend: one implementation per platform draws the 256x240
// BGRA picture nearest-neighbour into a destination rectangle (integer / FILL scale, 8:7 and the
// overscan crop come from computeGameRect), Dear ImGui on top, presents, and reports when each
// present reached the screen (the pacing source of the frame loop, docs/FRAME_PACING.md):
//   Linux    VkRenderer (apps/linux/src/vk_renderer.h): Vulkan FIFO + VK_KHR_present_wait
//   Windows  D3D11Renderer (apps/windows/src/d3d11_renderer.h): flip-model swap chain, frame
//            latency waitable object + DXGI frame statistics
// The display post-process (CRT, render/post_process.h) is part of the renderer; a renderer
// without it reports crtAvailable = false.
// Frame-loop thread only (takeCompleted() included).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "render/crt_settings.h"
#include "render/post_process.h"

struct ImDrawData;

namespace rnl {

struct PresentDone {
  uint64_t id = 0;
  double time = 0;  // nowSeconds() (host_clock.h) when the picture reached the screen (vblank)
  bool ok = false;  // false: never confirmed (timeout, swapchain recreated, hidden window)
  int result = 0;   // platform result code of the wait (diagnostics)
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

class Renderer {
 public:
  virtual ~Renderer() = default;

  /// Device + swap chain for the SDL window (created with windowFlags()).
  virtual bool init(SDL_Window* window, std::string* error) = 0;
  /// The Dear ImGui platform (SDL3) + renderer backends (after ImGui::CreateContext).
  virtual bool initImGui() = 0;
  /// Shuts the renderer backend down (ImGui's SDL3 backend is shut down by the caller first).
  virtual void shutdown() = 0;
  /// The renderer backend's ImGui::NewFrame part (before ImGui_ImplSDL3_NewFrame).
  virtual void newImGuiFrame() = 0;

  /// Present timestamps are available: the loop aims frames at vblanks (just-in-time input).
  /// false: host-clock pacing at the NES rate (also a renderer's own choice, e.g. VRR).
  virtual bool presentTiming() const = 0;
  virtual std::string description() const = 0;
  /// Drawable size in pixels (0 x 0 before the first present).
  virtual int width() const = 0;
  virtual int height() const = 0;

  /// Draws (newPicture: upload it first; nullptr keeps the last one) and presents. Returns the
  /// present id (0 when nothing was presented, e.g. a minimised window). `signal`: the CRT side
  /// channel of newPicture (render/post_process.h).
  virtual uint64_t drawAndPresent(const uint32_t* newPicture, const GameRect& rect, ImDrawData* ui,
                                  const FrameSignal* signal = nullptr) = 0;
  /// Presents confirmed on screen since the last call, in present order.
  virtual std::vector<PresentDone> takeCompleted() = 0;
  /// Seconds the last drawAndPresent spent blocked (fence / image / swap chain).
  virtual double lastAcquireWait() const = 0;
  /// The display's refresh period measured by the presenter itself (vblank counts over seconds;
  /// Windows: DXGI frame statistics); 0 = unknown (the loop estimates it from present timestamps).
  virtual double refreshPeriod() const { return 0; }

  /// Display post-process (CRT on/off + parameters). Takes effect with the next draw.
  virtual void setPostProcess(const DisplayPostProcess& pp) = 0;
  virtual const DisplayPostProcess& postProcess() const = 0;
  virtual PostProcessStatus postProcessStatus() const = 0;
  /// Seconds the GPU may spend on a new picture before its target vblank at the maximum input
  /// lead (CRT build-ahead decision; 0 = unknown).
  virtual void setGpuBudget(double seconds) { (void)seconds; }
  /// GPU time a new picture's build adds before it can be shown (part of the sample -> screen work).
  virtual double gpuLeadExtra() const { return 0; }

  /// Display watchdog (display_health.h, docs/FRAME_PACING.md "Display watchdog"). The last
  /// drawAndPresent presented and the CRT build of its picture (CRT on) did not fail.
  virtual bool lastPresentHealthy() const { return true; }
  /// The window is covered (e.g. DXGI_STATUS_OCCLUDED): nothing is expected on screen.
  virtual bool occluded() const { return false; }
  /// restart = false (RECOVER): the CRT output and temporal state are discarded and the next present
  /// shows the plain picture. restart = true (RESTART): the CRT resources and the swap chain are
  /// recreated (the device too after a device loss).
  virtual void recoverDisplay(bool restart) { (void)restart; }
  /// One line for the log: display failures and the last one, device / swap chain and CRT state.
  virtual std::string displayHealth() const { return std::string(); }

  /// Once per UI frame, before thumbTexture() calls.
  virtual void beginUIFrame() = 0;
  /// Filmstrip thumbnails: a 128x120 BGRA picture identified by `key` in the thumbnail atlas
  /// (uploaded before this frame's draw if new). false: no room this frame (draw a placeholder).
  virtual bool thumbTexture(uint64_t key, const uint32_t* px128x120, uint64_t* texId, float uv[4]) = 0;
  /// Saves the next presented frame (game + UI) as PNG (tests, docs screenshots).
  virtual void requestScreenshot(const std::string& path) = 0;
};

}  // namespace rnl
