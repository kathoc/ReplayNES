// Where the game picture goes in a drawable (shared by the renderers).
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>

#include "game_rect.h"
#include "replaynes/product.h"
#include "replaynes/replaynes.h"

namespace rnl {

GameRect computeGameRect(int width, int height, bool integerScale, bool par87, bool hideOverscan) {
  hideOverscan = RN_PRODUCT_OVERSCAN_CROP && hideOverscan;
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

GameRect computeGameRect(int width, int height, const rn_media_info& media,
                         const rn_video_frame_info& frame, bool integerScale, bool gameAspect, bool hideOverscan) {
  hideOverscan = RN_PRODUCT_OVERSCAN_CROP && hideOverscan;
  const bool nes = media.max_width == RN_VIDEO_WIDTH && media.max_height == RN_VIDEO_HEIGHT &&
                   media.rotation == 0 && media.pixel_aspect_num == 8 && media.pixel_aspect_den == 7 &&
                   frame.width == RN_VIDEO_WIDTH && frame.height == RN_VIDEO_HEIGHT;
  if (nes) return computeGameRect(width, height, integerScale, gameAspect, hideOverscan);
  GameRect r;
  if (width <= 0 || height <= 0 || !frame.width || !frame.height) return r;
  r.crop = hideOverscan ? std::min(8, int((std::min(frame.width, frame.height) - 1) / 2)) : 0;
  double par = gameAspect && media.pixel_aspect_num && media.pixel_aspect_den
                   ? double(media.pixel_aspect_num) / media.pixel_aspect_den : 1.0;
  double sw = (frame.width - 2 * r.crop) * par, sh = frame.height - 2 * r.crop;
  if (media.rotation & 1) std::swap(sw, sh);
  double scale = std::min(width / sw, height / sh);
  if (integerScale && scale >= 1) scale = std::floor(scale);
  r.w = float(std::round(sw * scale));
  r.h = float(std::round(sh * scale));
  r.x = float(std::round((width - r.w) / 2));
  r.y = float(std::round((height - r.h) / 2));
  r.visible = r.w > 0 && r.h > 0;
  return r;
}

}  // namespace rnl
