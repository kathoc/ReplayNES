// Frame-loop instrumentation (the Linux counterpart of the macOS --stats-log / --frame-log and
// scripts/perf-smoke.sh analysis; docs/FRAME_PACING.md section 1):
//   wake -> input sample (JIT wait), input sample -> submit (emulate + flash filter + UI + record),
//   submit -> on screen, INPUT SAMPLE -> ON SCREEN, on screen - target vblank,
//   input event -> on screen (injected or real game-button presses),
//   judder (a new picture's time on screen differs from what its cadence intends by > 1/2
//   refresh), missed targets, pictures never confirmed on screen, audio underruns / DRC ratio,
//   CPU (process, frame-loop thread).
// "On screen" = vkWaitForPresentKHR returned for that present (just after the flip).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rnl {

struct FrameRecord {
  uint64_t seq = 0;       // loop iteration
  uint64_t presentId = 0; // Vulkan present id (0 = not presented)
  uint64_t frame = 0;     // rn_frame after the tick
  bool emulated = false;  // a frame was emulated with live input (new picture)
  double wake = 0;        // loop woke (after the previous present completed)
  double sample = 0;      // input sampled (events pumped)
  double polled = 0;      // SDL events pumped
  double emulatedAt = 0; // emulation + flash filter + audio push done
  double ui = 0;          // UI built (ImGui::Render)
  double acquireWait = 0; // seconds blocked in fence wait + vkAcquireNextImageKHR
  double submit = 0;      // command buffer submitted + present queued
  double target = 0;      // vblank aimed at (0 = unknown)
  double displayed = 0;   // present completed (0 = never confirmed)
  double event = 0;       // game-button event this frame's sample picked up (0 = none)
  double lead = 0;        // input lead used (s)
  double refresh = 0;     // refresh estimate (s)
  int frames = 1;         // frames emulated for this present (2 on displays slower than 60.0988 Hz)
  double gpuExtra = 0;    // CRT build GPU time counted into the work (s)
};

struct RunInfo {
  std::string label, videoDriver, gpu, audioDevice, cadence, mode;
  int width = 0, height = 0;
  bool fullscreen = false, presentWait = false;
  uint64_t backlogDrains = 0;
  uint64_t multiFramePresents = 0, droppedFrames = 0;  // displays slower than the NES rate
  std::string crt = "off";                             // CRT display status
};

class PerfStats {
 public:
  void add(const FrameRecord& r) { frames_.push_back(r); }
  /// Marks the start of the measured window (after warm-up).
  void beginWindow(double now, double cpuProcess, double cpuThread, uint64_t underruns, uint64_t emulated);
  void endWindow(double now, double cpuProcess, double cpuThread, uint64_t underruns, uint64_t emulated,
                 double audioRatio, double audioFillMs, uint64_t skippedRefreshes);
  bool inWindow() const { return begun_ && !ended_; }
  /// Writes the summary (JSON, one object) and the per-frame CSV (if path non-empty); returns a
  /// human-readable summary.
  std::string finish(const RunInfo& info, const std::string& statsPath, const std::string& framePath);

 private:
  std::vector<FrameRecord> frames_;
  bool begun_ = false, ended_ = false;
  double t0_ = 0, t1_ = 0, cpuP0_ = 0, cpuP1_ = 0, cpuT0_ = 0, cpuT1_ = 0;
  uint64_t under0_ = 0, under1_ = 0, emu0_ = 0, emu1_ = 0, skipped_ = 0;
  double ratio_ = 1, fillMs_ = 0;
};

}  // namespace rnl
