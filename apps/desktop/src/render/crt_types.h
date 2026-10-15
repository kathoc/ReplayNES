// Graphics-API-neutral types of the CRT pipeline, shared by its two GPU ports (the same passes,
// constants and parameters as the macOS Metal version):
//   Vulkan       rnl::CrtRenderer       apps/linux/src/render/crt_renderer.h (GLSL, SPIR-V at build time)
//   Direct3D 11  rnl::CrtRendererD3D11  apps/windows/src/crt_d3d11.h (HLSL generated from the same GLSL)
// and their shared host logic: render/crt_display_policy.h (live view), render/crt_conformance.h
// (conformance test against tests/fixtures/crt/reference.json).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace rnl {

/// noSignal: no picture at all (the machine is off): the RF path with black codes, for a reception
/// with no carrier (snow; see CrtReception).
enum class CrtInputKind { codes, rgb, drive, noSignal };

/// ReplayNES reception around the picture (docs/CRT_PORT.md "No signal and power-on"): the receiver
/// settings of rnf_crt_reception_receiver (frontend core) plus the deflection of an unlocked sync.
/// No reception (CrtInput::reception nullptr, the default) = nesterm's model untouched. Display
/// only. Mirrors CRTRenderer.Reception (macOS).
struct CrtReception {
  float signalLevel = 1;                       // RF carrier level (0 = noise only)
  float minGain = 1.0f / 16, maxGain = 16.0f;  // AGC limits (equal = pinned)
  uint32_t noiseFrame = 0;                     // RF noise key frame (instead of the ordinal)
  float chroma = 1, lift = 0;                  // colour killer gain, detector DC lift (luma)
  float roll = 0, tear = 0, tearCycles = 0, tearPhase = 0;
  float jitter = 0;
  uint32_t jitterKey = 0;                      // deflection (rx_deflect)
  float picture = 1;                           // gain of input without an RF path (RGB)
  bool noSignal = false;                       // no carrier at all (snow)
  bool deflects() const { return roll != 0 || tear != 0 || jitter != 0; }
};

/// The reception of power-on step `step` (0 .. RNF_CRT_POWER_ON_FRAMES-1) or
/// RNF_CRT_RECEPTION_NO_SIGNAL at picture `counter` (rnf_crt_reception_at +
/// rnf_crt_reception_receiver at the antenna's carrier-to-noise ratio). False = normal reception
/// (no reception: the model untouched); *out is then left as is. Implemented in crt_model.cpp.
bool crtReceptionAt(int step, uint64_t counter, bool flashReduced, double antennaDbuv, CrtReception* out);

/// `reference`: the 1:1 port (conformance tests, MP4 export). `fast`: the live display path - the
/// same model with restructured kernels and approximations far below 8-bit display precision
/// (docs/CRT_PORT.md "Fast path"; bounded against the reference by the conformance harness).
/// Takes effect at the next configure(). Mirrors CRTRenderer.Quality (macOS).
enum class CrtQuality { reference, fast };

/// One emulated frame for the CRT.
struct CrtInput {
  CrtInputKind kind = CrtInputKind::codes;
  const uint16_t* codes = nullptr;  // 256x240 9-bit PPU codes (rn_video_indices): the RF path
  uint32_t burstPhase = 0;          // Nestopia colour-burst phase of that frame
  const uint32_t* rgb = nullptr;    // 256x240 BGRA8 (nesterm's synthetic-RGB "ascii" path: no RF)
  const float* drive = nullptr;     // 512x240 RGBA float linear drive (verification)
  /// No signal / power-on (nullptr: normal reception, the model untouched). Its frames continue the
  /// history (ordinal = previous + 1, whatever the ordinal given) so the snow, the AGC and the
  /// phosphors run on from picture to picture.
  const CrtReception* reception = nullptr;
};

/// Stage buffers the verification hooks read back (floats, RGBA per pixel).
enum class CrtStage { receiver, tubeInput, emission, output };

/// Tube output size that maps 1:1 onto a destination rectangle of `dst` pixels showing the rows
/// between the overscan crops, capped at `maxWidth` (4:3 full raster) - CRTRenderer.tubeSize.
void crtTubeSize(double dstWidth, double dstHeight, double cropFraction, int maxWidth, int* w, int* h);

}  // namespace rnl
