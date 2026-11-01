// Picture placement, independent of the presentation backend.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "render/crt_settings.h"
#include "replaynes/replaynes.h"

namespace rnl {

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
GameRect computeGameRect(int width, int height, const rn_media_info& media,
                         const rn_video_frame_info& frame, bool integerScale, bool gameAspect, bool hideOverscan);

/// The bezel (docs/design/BEZEL.md) fitted into a screen: the picture's place (aspect kept, centred)
/// and its window's, scaled the same way. Empty (w = 0) when nothing fits.
struct BezelLayout {
  float x = 0, y = 0, w = 0, h = 0;       // the bezel picture
  float wx = 0, wy = 0, ww = 0, wh = 0;   // its window
};
BezelLayout placeBezel(int screenW, int screenH, int imageW, int imageH, int winX, int winY, int winW, int winH);
/// `rect`, laid out in a window of the size `b.ww` x `b.wh`, moved to that window's place.
GameRect moveIntoBezel(GameRect rect, const BezelLayout& b);

}  // namespace rnl
