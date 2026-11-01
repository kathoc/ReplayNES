// QOI to and from RGBA (qoiformat.org, specification 1.0).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "qoi.h"

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

void encodeQoi(const uint8_t* rgba, uint32_t w, uint32_t h, std::vector<uint8_t>* out) {
  auto put32 = [&](uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out->push_back(uint8_t(v >> s));
  };
  out->insert(out->end(), {'q', 'o', 'i', 'f'});
  put32(w);
  put32(h);
  out->push_back(4);
  out->push_back(0);
  uint8_t index[64][4] = {};
  uint8_t prev[4] = {0, 0, 0, 255};
  int run = 0;
  const size_t pixels = size_t(w) * h;
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t* px = rgba + i * 4;
    if (std::memcmp(px, prev, 4) == 0) {
      if (++run == 62 || i + 1 == pixels) {
        out->push_back(uint8_t(0xC0 | (run - 1)));  // QOI_OP_RUN
        run = 0;
      }
      continue;
    }
    if (run > 0) {
      out->push_back(uint8_t(0xC0 | (run - 1)));
      run = 0;
    }
    const int slot = (px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64;
    if (std::memcmp(index[slot], px, 4) == 0) {
      out->push_back(uint8_t(slot));  // QOI_OP_INDEX
    } else {
      std::memcpy(index[slot], px, 4);
      if (px[3] == prev[3]) {
        const int dr = int8_t(px[0] - prev[0]), dg = int8_t(px[1] - prev[1]), db = int8_t(px[2] - prev[2]);
        const int drg = dr - dg, dbg = db - dg;
        if (dr > -3 && dr < 2 && dg > -3 && dg < 2 && db > -3 && db < 2) {
          out->push_back(uint8_t(0x40 | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2)));  // QOI_OP_DIFF
        } else if (drg > -9 && drg < 8 && dg > -33 && dg < 32 && dbg > -9 && dbg < 8) {
          out->push_back(uint8_t(0x80 | (dg + 32)));  // QOI_OP_LUMA
          out->push_back(uint8_t((drg + 8) << 4 | (dbg + 8)));
        } else {
          out->insert(out->end(), {0xFE, px[0], px[1], px[2]});  // QOI_OP_RGB
        }
      } else {
        out->insert(out->end(), {0xFF, px[0], px[1], px[2], px[3]});  // QOI_OP_RGBA
      }
    }
    std::memcpy(prev, px, 4);
  }
  out->insert(out->end(), {0, 0, 0, 0, 0, 0, 0, 1});
}

}  // namespace rnl
