// SPDX-License-Identifier: GPL-2.0-or-later
#include "export_frame.h"

#include <algorithm>
#include <cstring>

#include "mp4_export.h"

namespace rnl {

rnf_export_settings defaultExportSettings() {
  rnf_export_settings s{};
  s.preset.kind = RNF_PRESET_CANVAS;
  s.preset.width = 1280;
  s.preset.height = 960;
  s.crop_top = 8;
  s.crop_bottom = 8;
  return s;
}

namespace exportframe {

namespace {
constexpr int kFrameW = RN_VIDEO_WIDTH;
// ------------------------------------------------------------------ BGRA -> BT.709 limited range
// Exact integer conversion (16.16 fixed point, rounded). Coefficients are the BT.709 matrix scaled
// to 219 (luma) / 224 (chroma) steps; each chroma row sums to 0 so greys get Cb = Cr = 128, and
// white maps to Y 235, black to Y 16. Chroma is the mean of the 2x2 block (4:2:0, centred).
struct Coeffs {
  int yr, yg, yb, ur, ug, ub, vr, vg, vb;
};
constexpr int fix(double v) { return int(v * 65536.0 + (v < 0 ? -0.5 : 0.5)); }
constexpr Coeffs makeCoeffs() {
  const double kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb, ys = 219.0 / 255.0, cs = 224.0 / 255.0;
  Coeffs c{};
  c.yr = fix(kr * ys);
  c.yb = fix(kb * ys);
  c.yg = fix(ys) - c.yr - c.yb;  // sum = 219/255 exactly as rounded
  c.ur = fix(-kr / (2 * (1 - kb)) * cs);
  c.ug = fix(-kg / (2 * (1 - kb)) * cs);
  c.ub = -c.ur - c.ug;
  c.vg = fix(-kg / (2 * (1 - kr)) * cs);
  c.vb = fix(-kb / (2 * (1 - kr)) * cs);
  c.vr = -c.vg - c.vb;
  return c;
}
constexpr Coeffs kC = makeCoeffs();

inline uint8_t lumaOf(uint32_t p) {
  const int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
  return uint8_t((kC.yr * r + kC.yg * g + kC.yb * b + (16 << 16) + (1 << 15)) >> 16);
}

}  // namespace

void convertToYuv(const uint8_t* canvas, size_t stride, int w, int h, uint8_t* y, int yStride, uint8_t* u, int uStride,
                  uint8_t* v, int vStride) {
  for (int row = 0; row < h; row += 2) {
    const uint32_t* s0 = reinterpret_cast<const uint32_t*>(canvas + size_t(row) * stride);
    const uint32_t* s1 = reinterpret_cast<const uint32_t*>(canvas + size_t(row + 1) * stride);
    uint8_t* y0 = y + size_t(row) * yStride;
    uint8_t* y1 = y0 + yStride;
    uint8_t* uRow = u + size_t(row / 2) * uStride;
    uint8_t* vRow = v ? v + size_t(row / 2) * vStride : nullptr;
    // Identical row pair as the previous pair (the common case for scaled pixel art): copy.
    if (row >= 2 && std::memcmp(s0, s0 - 2 * stride / 4, size_t(w) * 4) == 0 &&
        std::memcmp(s1, s1 - 2 * stride / 4, size_t(w) * 4) == 0) {
      std::memcpy(y0, y0 - 2 * size_t(yStride), size_t(w));
      std::memcpy(y1, y1 - 2 * size_t(yStride), size_t(w));
      std::memcpy(uRow, uRow - uStride, v ? size_t(w / 2) : size_t(w));
      if (vRow) std::memcpy(vRow, vRow - vStride, size_t(w / 2));
      continue;
    }
    for (int x = 0; x < w; x += 2) {
      const uint32_t a = s0[x], b = s0[x + 1], c = s1[x], d = s1[x + 1];
      y0[x] = lumaOf(a);
      y0[x + 1] = lumaOf(b);
      y1[x] = lumaOf(c);
      y1[x + 1] = lumaOf(d);
      const int bs = int(a & 0xFF) + int(b & 0xFF) + int(c & 0xFF) + int(d & 0xFF);
      const int gs = int((a >> 8) & 0xFF) + int((b >> 8) & 0xFF) + int((c >> 8) & 0xFF) + int((d >> 8) & 0xFF);
      const int rs = int((a >> 16) & 0xFF) + int((b >> 16) & 0xFF) + int((c >> 16) & 0xFF) + int((d >> 16) & 0xFF);
      const uint8_t cb = uint8_t((kC.ur * rs + kC.ug * gs + kC.ub * bs + (128 << 18) + (1 << 17)) >> 18);
      const uint8_t cr = uint8_t((kC.vr * rs + kC.vg * gs + kC.vb * bs + (128 << 18) + (1 << 17)) >> 18);
      if (vRow) {
        uRow[x / 2] = cb;
        vRow[x / 2] = cr;
      } else {
        uRow[x] = cb;
        uRow[x + 1] = cr;
      }
    }
  }
}

// ------------------------------------------------------------------ nearest scaling (MP4Exporter.scale)
// Rows are built once and copied for vertical repeats.
void scaleNearest(const uint32_t* src, uint8_t* canvas, size_t stride, const rnf_export_geometry& g,
                  const std::vector<int>& cols, const std::vector<int>& rows) {
  const uint32_t black = 0xFF000000u;  // BGRA little-endian, A = 255
  int lastSrcRow = -1;
  const uint32_t* lastDstRow = nullptr;
  for (int y = 0; y < g.canvas_height; ++y) {
    uint32_t* row = reinterpret_cast<uint32_t*>(canvas + size_t(y) * stride);
    const int dy = y - g.dst_y;
    if (dy < 0 || dy >= g.dst_height) {
      std::fill(row, row + g.canvas_width, black);
      continue;
    }
    const int sy = rows[size_t(dy)];
    if (sy == lastSrcRow && lastDstRow) {
      std::memcpy(row, lastDstRow, size_t(g.canvas_width) * 4);
      continue;
    }
    if (g.dst_x > 0) std::fill(row, row + g.dst_x, black);
    const int right = g.dst_x + g.dst_width;
    if (right < g.canvas_width) std::fill(row + right, row + g.canvas_width, black);
    const uint32_t* srow = src + size_t(sy) * kFrameW;
    uint32_t* out = row + g.dst_x;
    for (int x = 0; x < g.dst_width; ++x) out[x] = srow[cols[size_t(x)]] | black;
    lastSrcRow = sy;
    lastDstRow = row;
  }
}


int64_t frameTime100ns(uint64_t n) {
  // n * 655171 * 10^7 / 39375000 = n * 655171 * 16 / 63 (exact reduction), rounded to nearest.
  // No overflow below 8.8e11 frames (~460 years).
  const uint64_t num = n * (uint64_t(RN_FPS_DEN) * 16);
  return int64_t((num + 31) / 63);
}

int64_t sampleTime100ns(uint64_t n) { return int64_t((n * 625 + 1) / 3); }

}  // namespace exportframe
}  // namespace rnl
