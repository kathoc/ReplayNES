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

enum class CrtInputKind { codes, rgb, drive };

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
};

/// Stage buffers the verification hooks read back (floats, RGBA per pixel).
enum class CrtStage { receiver, tubeInput, emission, output };

/// Tube output size that maps 1:1 onto a destination rectangle of `dst` pixels showing the rows
/// between the overscan crops, capped at `maxWidth` (4:3 full raster) - CRTRenderer.tubeSize.
void crtTubeSize(double dstWidth, double dstHeight, double cropFraction, int maxWidth, int* w, int* h);

}  // namespace rnl
