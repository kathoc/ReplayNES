// QOI to RGBA (qoiformat.org, specification 1.0).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "qoi_decode.h"

#include <cstring>

namespace rnl {

namespace {
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
}  // namespace

bool decodeQoi(const uint8_t* data, size_t size, uint32_t* width, uint32_t* height, std::vector<uint8_t>* rgba) {
  constexpr size_t kHeader = 14, kEnd = 8;
  if (!data || size < kHeader + kEnd || std::memcmp(data, "qoif", 4) != 0) return false;
  const uint32_t w = be32(data + 4), h = be32(data + 8);
  if (!w || !h || uint64_t(w) * h > (uint64_t(1) << 28)) return false;  // 256 M pixels at most
  const size_t pixels = size_t(w) * h;
  rgba->assign(pixels * 4, 0);
  uint8_t index[64][4] = {};
  uint8_t px[4] = {0, 0, 0, 255};
  size_t p = kHeader, run = 0;
  const size_t end = size - kEnd;
  for (size_t i = 0; i < pixels; ++i) {
    if (run > 0) {
      --run;
    } else {
      if (p >= end) return false;
      const uint8_t b = data[p++];
      if (b == 0xFE) {  // QOI_OP_RGB
        if (p + 3 > end) return false;
        px[0] = data[p], px[1] = data[p + 1], px[2] = data[p + 2];
        p += 3;
      } else if (b == 0xFF) {  // QOI_OP_RGBA
        if (p + 4 > end) return false;
        std::memcpy(px, data + p, 4);
        p += 4;
      } else if ((b & 0xC0) == 0x00) {  // QOI_OP_INDEX
        std::memcpy(px, index[b], 4);
      } else if ((b & 0xC0) == 0x40) {  // QOI_OP_DIFF
        px[0] = uint8_t(px[0] + ((b >> 4) & 3) - 2);
        px[1] = uint8_t(px[1] + ((b >> 2) & 3) - 2);
        px[2] = uint8_t(px[2] + (b & 3) - 2);
      } else if ((b & 0xC0) == 0x80) {  // QOI_OP_LUMA
        if (p >= end) return false;
        const uint8_t b2 = data[p++];
        const int dg = (b & 0x3F) - 32;
        px[0] = uint8_t(px[0] + dg - 8 + ((b2 >> 4) & 0x0F));
        px[1] = uint8_t(px[1] + dg);
        px[2] = uint8_t(px[2] + dg - 8 + (b2 & 0x0F));
      } else {  // QOI_OP_RUN: this pixel and (b & 0x3F) more
        run = b & 0x3F;
      }
      std::memcpy(index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64], px, 4);
    }
    std::memcpy(rgba->data() + i * 4, px, 4);
  }
  *width = w;
  *height = h;
  return true;
}

}  // namespace rnl
