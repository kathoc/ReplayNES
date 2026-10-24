// Owned, tightly packed pictures for presentation caches.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <limits>
#include <vector>
#include "replaynes/frontend.h"

namespace rnf {
inline bool validPicture(const rn_video_frame_info* f, uint32_t mw, uint32_t mh) {
  return f && f->pixels && f->width && f->height && f->width <= mw && f->height <= mh &&
         f->stride >= f->width && size_t(f->height) <= SIZE_MAX / f->stride / sizeof(uint32_t);
}
struct Picture {
  std::vector<uint32_t> pixels;
  uint32_t width = 0, height = 0;
  void assign(const rn_video_frame_info& f) {
    pixels.resize(size_t(f.width) * f.height);
    for (uint32_t y = 0; y < f.height; ++y)
      std::copy_n(f.pixels + size_t(y) * f.stride, f.width, pixels.data() + size_t(y) * f.width);
    width = f.width;
    height = f.height;
  }
  rn_video_frame_info view() const { return {pixels.data(), width, height, width}; }
};
}  // namespace rnf
