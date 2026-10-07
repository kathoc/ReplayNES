// Where the game picture goes in a drawable (shared by the renderers).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>

#include "renderer.h"
#include "replaynes/replaynes.h"

namespace rnl {

GameRect computeGameRect(int width, int height, bool integerScale, bool par87, bool hideOverscan) {
  GameRect r;
  if (width <= 0 || height <= 0) return r;
  r.crop = hideOverscan ? 8 : 0;
  double srcH = RN_VIDEO_HEIGHT - 2 * r.crop, srcW = RN_VIDEO_WIDTH - 2 * r.crop;
  double par = par87 ? 8.0 / 7.0 : 1.0;
  double scale = std::min(width / (srcW * par), height / srcH);
  if (integerScale && scale >= 1) scale = std::floor(scale);
  double dw = std::round(srcW * par * scale), dh = std::round(srcH * scale);
  r.x = float(std::round((width - dw) / 2));
  r.y = float(std::round((height - dh) / 2));
  r.w = float(dw);
  r.h = float(dh);
  r.visible = dw > 0 && dh > 0;
  // CRT: full 4:3 raster minus the cropped rows, at the plain (1:1 pixel) viewport's height.
  r.crtCrop = (hideOverscan ? 8.0 : 0.0) / 240;
  double aspect = (4.0 / 3.0) / (1 - 2 * r.crtCrop);
  double ps = std::min(width / srcW, height / srcH);
  if (integerScale && ps >= 1) ps = std::floor(ps);
  double ch = std::round(srcH * ps), cw = std::round(ch * aspect);
  if (cw > width) { cw = width; ch = std::round(cw / aspect); }
  r.crt = CrtRect{float(std::round((width - cw) / 2)), float(std::round((height - ch) / 2)), float(cw), float(ch)};
  return r;
}

}  // namespace rnl
