// The blurred backdrop (docs/design/BACKDROP.md): where the game picture leaves the screen empty
// (pillarbox / letterbox), the same picture enlarged to cover the screen, blurred and dimmed behind it.
// Display only. The renderers share this plan; each blurs with the picture's mip chain read at `lod`.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>

namespace rnl {

struct BackdropPlan {
  bool show = false;
  float x = 0, y = 0, w = 0, h = 0;  // the covering rectangle (pixels, origin top-left; beyond the screen)
  float lod = 0;                     // mip level to read: its long side is about kBackdropTexels
  float dim = 0;                     // brightness factor
};

constexpr float kBackdropTexels = 12.f;  // the blur: the picture seen through about 12 texels across (user: "more clearly blurred")
constexpr float kBackdropDim = 0.6f;     // slightly darker than the picture (user decision 2026-11-01)
// Contrast 40 % lower: toward the picture's mean colour (the mip chain's last level), so a hard flash of
// the picture does not flash the whole screen as hard (user decision 2026-11-01). The shaders use it.
constexpr float kBackdropContrast = 0.6f;

/// `pic*`: the rectangle the picture is drawn in; `srcW` x `srcH`: the backdrop texture's level 0.
inline BackdropPlan planBackdrop(float picX, float picY, float picW, float picH, int screenW, int screenH, int srcW,
                                 int srcH) {
  BackdropPlan p;
  if (picW < 1 || picH < 1 || screenW < 1 || screenH < 1 || srcW < 1 || srcH < 1) return p;
  // Something of the screen left empty (more than a pixel).
  const bool empty = picX > 1 || picY > 1 || picX + picW < screenW - 1 || picY + picH < screenH - 1;
  if (!empty) return p;
  const float s = std::max(screenW / picW, screenH / picH);
  p.w = picW * s;
  p.h = picH * s;
  p.x = (screenW - p.w) * 0.5f;
  p.y = (screenH - p.h) * 0.5f;
  p.lod = std::max(0.f, std::log2(float(std::max(srcW, srcH)) / kBackdropTexels));
  p.dim = kBackdropDim;
  p.show = true;
  return p;
}

/// The plan's rectangle cut to the screen (a viewport stays within the device's limits), and the
/// source rectangle `uv` (u0, v0, u1, v1) cut alike. `rotation` 0..3 as the picture's quad maps it:
/// with p the position in the rectangle (0..1, origin top-left), the source is at q = p (0),
/// (p.y, 1 - p.x) (1), 1 - p (2), (1 - p.y, p.x) (3).
inline void cropBackdrop(BackdropPlan& p, int screenW, int screenH, int rotation, float uv[4]) {
  if (!p.show) return;
  const float ax = std::max(0.f, -p.x / p.w), bx = std::min(1.f, (screenW - p.x) / p.w);
  const float ay = std::max(0.f, -p.y / p.h), by = std::min(1.f, (screenH - p.y) / p.h);
  float qx0 = ax, qx1 = bx, qy0 = ay, qy1 = by;  // the cut rectangle in source space
  switch (rotation & 3) {
    case 1: qx0 = ay, qx1 = by, qy0 = 1 - bx, qy1 = 1 - ax; break;
    case 2: qx0 = 1 - bx, qx1 = 1 - ax, qy0 = 1 - by, qy1 = 1 - ay; break;
    case 3: qx0 = 1 - by, qx1 = 1 - ay, qy0 = ax, qy1 = bx; break;
    default: break;
  }
  const float u0 = uv[0], v0 = uv[1], du = uv[2] - uv[0], dv = uv[3] - uv[1];
  uv[0] = u0 + du * qx0;
  uv[2] = u0 + du * qx1;
  uv[1] = v0 + dv * qy0;
  uv[3] = v0 + dv * qy1;
  const float x = p.x + ax * p.w, y = p.y + ay * p.h;
  p.w *= bx - ax;
  p.h *= by - ay;
  p.x = x;
  p.y = y;
}

}  // namespace rnl
