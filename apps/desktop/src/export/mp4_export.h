// Offline MP4 export of the desktop frontend (port of apps/macos/Sources/Core/MP4Exporter.swift):
// drives an rn_renderer (fresh core, logical time) and encodes H.264 + AAC
//   Linux    FFmpeg's libav* (apps/linux/src/export/mp4_export.cpp)
//   Windows  Media Foundation's sink writer (apps/windows/src/mp4_export_mf.cpp)
// Timestamps come only from frame / sample counts, never from the wall clock:
//   video PTS of frame f = (f - start) * 655171 in time base 1/39375000 (FFmpeg: exact, the MP4
//   track timescale is 39375000; Media Foundation: the same instants in its 100 ns units, rounded),
//   audio PTS = rn_audio_samples_before(f) - rn_audio_samples_before(start) at 48 kHz mono.
// Shared by both: the frame helpers (export_frame.h) and ExportJob (export_job.cpp).
// Geometry (presets, crops, 8:7, nearest-neighbour column/row maps, bit rate) comes from the shared
// frontend core (frontend.h, "export + streaming geometry"). UI-free: replaynes-export (export_cli.cpp)
// exercises it headlessly.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "replaynes/frontend.h"
#include "replaynes/replaynes.h"

namespace rnl {

/// CRT side channel of one exported frame (display only).
struct ExportFrameSignal {
  const uint16_t* codes = nullptr;  // rn_video_indices codes (nullptr: core has none)
  uint32_t burstPhase = 0;
  uint64_t ordinal = 0;             // machine frame ordinal (info.frame; the take frame when the core has no codes)
  bool flashAltered = false;        // the flash filter changed this frame (use the RGB picture, not the codes)
};

/// Optional offline post-process (CRT), implemented elsewhere. Called on the export thread, frames
/// strictly in order.
class ExportVideoProcessor {
 public:
  virtual ~ExportVideoProcessor() = default;
  /// Canvas size + the exporter geometry and settings; called once before the first frame.
  virtual bool begin(const rnf_export_settings& s, const rnf_export_geometry& g, std::string* error) = 0;
  /// pixels: 256x240 BGRA (already flash-filtered when enabled). Writes the whole canvas
  /// (g.canvas_width x g.canvas_height, BGRA, alpha 255) at canvasBGRA with `stride` bytes per row.
  virtual bool render(const ExportFrameSignal& sig, const uint32_t* pixels, uint8_t* canvasBGRA, size_t stride,
                      std::string* error) = 0;
};

/// Defaults like the macOS ExportSettings: canvas 1280x960, crop top/bottom 8, whole take.
rnf_export_settings defaultExportSettings();

struct ExportOptions {
  rnf_export_settings settings = defaultExportSettings();  // preset / crops / 8:7 / start / end
  /// Photosensitive flash reduction of the exported picture only (renderer, hash, project unaffected).
  rn_flash_level flash = RN_FLASH_OFF;
  int audioBitrate = 192000;
  double videoBitsPerPixel = 0.25;  // per frame; pixel art needs more than camera footage
  /// "" = auto. Linux: libx264, then h264_vaapi, then libopenh264 (first one that opens), or that
  /// FFmpeg encoder only. Windows: a hardware H.264 MFT when there is one, else Microsoft's
  /// software encoder; "mf_hardware" / "mf_software" force one.
  std::string encoder;
  /// Empty = plain nearest scaling (black borders). Called once on the export thread.
  std::function<std::unique_ptr<ExportVideoProcessor>(std::string* error)> makeProcessor;
};

struct ExportResult {
  uint64_t frames = 0, audioSamples = 0, rendererHash = 0;
  double duration = 0;  // frames * 655171 / 39375000 s
  std::string encoder;  // encoder that was used ("libx264", "h264_vaapi", "H264 Encoder MFT", ...)
};

/// The encoders the Export dialog offers besides "automatic" (id for ExportOptions::encoder, label).
struct ExportEncoderChoice {
  const char* id;
  const char* label;
};
const std::vector<ExportEncoderChoice>& exportEncoderChoices();

using ExportProgressFn = std::function<void(uint64_t done, uint64_t total)>;
using ExportCancelFn = std::function<bool()>;

/// Blocking; call on a worker thread. Takes ownership of `renderer` (frees it, also on failure).
/// Never touches the session / project. Writes outPath directly (an existing file is replaced).
/// On failure / cancel: removes the partial file, returns false with *error ("cancelled" on cancel).
/// progress / cancelled are called on the calling thread (every frame) and may be empty.
bool exportProject(rn_renderer* renderer, const ExportOptions& opt, const std::string& outPath,
                   const ExportProgressFn& progress, const ExportCancelFn& cancelled, ExportResult* result,
                   std::string* error);

/// The H.264 encoders auto mode tries, in order, and whether this FFmpeg build has each.
std::string availableEncodersDescription();

/// Convenience for the UI: creates the renderer from the session (rn_renderer_new on the caller's
/// thread, which must be the session's thread, as the engine requires) and runs exportProject on a
/// std::thread. Everything else is polled from the UI thread. Not copyable; the destructor cancels
/// and joins a running export.
class ExportJob {
 public:
  ExportJob() = default;
  ~ExportJob();
  ExportJob(const ExportJob&) = delete;
  ExportJob& operator=(const ExportJob&) = delete;

  /// false (error() set) when a job is still running or the renderer can't be created. A finished
  /// job may be started again.
  bool start(rn_session* session, ExportOptions opt, std::string outPath);
  void cancel();                      // asynchronous; finished() becomes true soon after
  bool running() const;               // started and not finished
  bool finished() const;              // the worker is done (success, failure or cancel)
  bool succeeded() const;             // finished without error
  bool wasCancelled() const;
  double progress() const;            // 0...1
  uint64_t framesDone() const { return done_.load(); }
  uint64_t framesTotal() const { return total_.load(); }
  ExportResult result() const;        // valid when succeeded()
  std::string error() const;          // "" while running / on success; "cancelled" after a cancel
  const std::string& outPath() const { return outPath_; }
  void wait();                        // joins the worker (blocks)

 private:
  std::thread worker_;
  std::atomic<bool> cancel_{false}, running_{false}, finished_{false}, ok_{false};
  std::atomic<uint64_t> done_{0}, total_{0};
  mutable std::mutex mutex_;
  ExportResult result_;
  std::string error_;
  std::string outPath_;
};

}  // namespace rnl
