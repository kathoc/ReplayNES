// CRT display parameters and the tube rectangle (no graphics API): shared by the settings UI, the
// display post-process interface (post_process.h) and the CRT renderers (Vulkan:
// apps/linux/src/render; Direct3D 11: apps/windows/src/crt_d3d11.h).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace rnl {

/// physical-worker.mjs `effects` + `lines` (defaults as shipped in nesterm's web UI).
struct CrtSettings {
  int lines = 240;           // 240 native, or 110...220 (reduced-line experiment)
  bool beamGrowth = true;    // scanlines thicker where brighter (M4a vertical + M4C-SPOT-H)
  bool persistence = true;   // phosphor persistence (M4b)
  bool supply = true;        // bright screens widen and dim the picture (M4c)
  double antennaDbuv = 65.0; // signal strength 20...90 dBuV (M3-NOISE)
  double ambientLux = 40.0;  // fixed in nesterm's UI

  static bool validLines(int l) { return l == 240 || (l >= 110 && l <= 220); }
  CrtSettings sanitized() const;
  bool operator==(const CrtSettings& o) const;
  bool operator!=(const CrtSettings& o) const { return !(*this == o); }
};

struct CrtRect { float x = 0, y = 0, w = 0, h = 0; };

}  // namespace rnl
