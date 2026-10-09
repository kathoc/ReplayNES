// Display-only presentation of the practice return and of rewinding (docs/design/UI_REDESIGN.md,
// "Practice: return to A" and "VTR effect"): the practice run's reel (an adaptively decimated set of
// pictures from A for the 1 s sweep back) and the VTR tape-rewind effect, a CPU pass on the 256x240
// picture that every frontend runs only while it is active. Neither touches emulation.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <vector>

#include "common.hpp"

namespace {
constexpr int kW = RN_VIDEO_WIDTH, kH = RN_VIDEO_HEIGHT;
constexpr size_t kPixels = size_t(kW) * size_t(kH);
}  // namespace

struct rnf_reel {
  int capacity = RNF_REEL_CAPACITY;
  uint64_t stride = 1;
  std::vector<uint32_t> storage;  // capacity frames
  std::vector<int> order;         // storage slots, oldest first
  std::vector<uint64_t> positions;
  std::vector<int> freeSlots;

  void reset() {
    stride = 1;
    order.clear();
    positions.clear();
    freeSlots.clear();
    for (int i = capacity - 1; i >= 0; --i) freeSlots.push_back(i);
  }
  bool onGrid(uint64_t position) const { return position >= 1 && (position - 1) % stride == 0; }
  // Full: every other frame goes (the ones off the doubled grid), the stride doubles.
  void decimate() {
    stride *= 2;
    size_t w = 0;
    for (size_t i = 0; i < order.size(); ++i) {
      if (onGrid(positions[i])) {
        order[w] = order[i];
        positions[w] = positions[i];
        ++w;
      } else {
        freeSlots.push_back(order[i]);
      }
    }
    order.resize(w);
    positions.resize(w);
  }
};

struct rnf_vtr {
  bool enabled = true;
  rn_flash_level level = RN_FLASH_STANDARD;
  float strength = 0;
  double start = 0, last = -1;
  rnf_vtr_kind kind = RNF_VTR_NONE;
};

namespace {

float levelScale(rn_flash_level l) {
  switch (l) {
    case RN_FLASH_OFF: return 1.0f;
    case RN_FLASH_LOW: return 0.85f;
    case RN_FLASH_STANDARD: return 0.6f;
    case RN_FLASH_HIGH: return 0.4f;
  }
  return 0.6f;
}

float kindPeak(rnf_vtr_kind k) {
  switch (k) {
    case RNF_VTR_REWIND: return 0.6f;        // in-game rewind: subtle, the picture stays readable
    case RNF_VTR_FAST_FORWARD: return 0.5f;  // the variant itself is lighter (thin bands only)
    case RNF_VTR_RETURN: return 1.0f;        // the practice sweep back to A
    case RNF_VTR_NONE: return 0.0f;
  }
  return 0.0f;
}

inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t h = a * 0x8da6b343u ^ b * 0xd8163841u ^ c * 0xcb1ab31fu;
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  h *= 0x297a2d39u;
  h ^= h >> 15;
  return h;
}
inline float unit(uint32_t h) { return float(h >> 8) * (1.0f / 16777216.0f); }

inline float clamp255(float v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

}  // namespace

extern "C" {

// ------------------------------------------------------------------ reel
rnf_reel* rnf_reel_new(int capacity) {
  try {
    auto* r = new rnf_reel;
    r->capacity = std::max(2, capacity);
    r->reset();
    return r;
  } catch (...) { return nullptr; }
}
void rnf_reel_free(rnf_reel* r) { delete r; }

int rnf_reel_offer(rnf_reel* r, uint64_t position, const uint32_t* frame) {
  if (!r || !frame || position == 0) return 0;
  // Positions only grow between truncations; an older one is not part of this run's reel.
  if (!r->positions.empty() && position <= r->positions.back()) return 0;
  if (!r->onGrid(position)) return 0;
  if (int(r->order.size()) >= r->capacity) {
    r->decimate();
    if (!r->onGrid(position)) return 0;
  }
  try {
    if (r->storage.empty()) r->storage.assign(size_t(r->capacity) * kPixels, 0);
  } catch (...) {
    return 0;
  }
  if (r->freeSlots.empty()) return 0;
  int slot = r->freeSlots.back();
  r->freeSlots.pop_back();
  std::copy(frame, frame + kPixels, r->storage.begin() + ptrdiff_t(size_t(slot) * kPixels));
  r->order.push_back(slot);
  r->positions.push_back(position);
  return 1;
}

void rnf_reel_truncate(rnf_reel* r, uint64_t position) {
  if (!r) return;
  while (!r->positions.empty() && r->positions.back() > position) {
    r->freeSlots.push_back(r->order.back());
    r->order.pop_back();
    r->positions.pop_back();
  }
}
void rnf_reel_clear(rnf_reel* r) {
  if (r) r->reset();
}
void rnf_reel_release(rnf_reel* r) {
  if (!r) return;
  r->reset();
  std::vector<uint32_t>().swap(r->storage);
}
int rnf_reel_count(const rnf_reel* r) { return r ? int(r->order.size()) : 0; }
int rnf_reel_capacity(const rnf_reel* r) { return r ? r->capacity : 0; }
uint64_t rnf_reel_stride(const rnf_reel* r) { return r ? r->stride : 1; }
uint64_t rnf_reel_position(const rnf_reel* r, int i) {
  if (!r || i < 0 || i >= int(r->positions.size())) return 0;
  return r->positions[size_t(i)];
}
const uint32_t* rnf_reel_frame(const rnf_reel* r, int i) {
  if (!r || i < 0 || i >= int(r->order.size()) || r->storage.empty()) return nullptr;
  return r->storage.data() + size_t(r->order[size_t(i)]) * kPixels;
}
int rnf_reel_sweep_index(const rnf_reel* r, double back) {
  int n = rnf_reel_count(r);
  if (n <= 0) return -1;
  double b = std::min(1.0, std::max(0.0, back));
  // Even in position (the kept frames are evenly spread): newest at 0, oldest at 1.
  int i = int(std::lround((1.0 - b) * double(n - 1)));
  return std::min(n - 1, std::max(0, i));
}

// ------------------------------------------------------------------ VTR effect
rnf_vtr* rnf_vtr_new(void) {
  try { return new rnf_vtr; } catch (...) { return nullptr; }
}
void rnf_vtr_free(rnf_vtr* v) { delete v; }

void rnf_vtr_configure(rnf_vtr* v, int enabled, rn_flash_level level) {
  if (!v) return;
  v->enabled = enabled != 0;
  v->level = level;
}

float rnf_vtr_peak(const rnf_vtr* v, rnf_vtr_kind kind) {
  if (!v || !v->enabled) return 0;
  return kindPeak(kind) * levelScale(v->level);
}

rnf_vtr_params rnf_vtr_tick(rnf_vtr* v, double now, rnf_vtr_kind want) {
  rnf_vtr_params p{};
  if (!v) return p;
  double dt = v->last < 0 ? 1.0 / 60 : std::min(0.1, std::max(0.0, now - v->last));
  v->last = now;
  float target = want == RNF_VTR_NONE ? 0.0f : rnf_vtr_peak(v, want);
  if (target > 0) {
    if (v->strength <= 0) v->start = now;
    v->kind = want;
  }
  if (target > v->strength) {
    v->strength = std::min(target, v->strength + float(dt / RNF_VTR_FADE_IN_SECONDS));
  } else if (target < v->strength) {
    v->strength = std::max(target, v->strength - float(dt / RNF_VTR_FADE_OUT_SECONDS));
  }
  if (v->strength <= 0.001f) v->strength = 0;
  p.strength = v->strength;
  p.time = v->strength > 0 ? now - v->start : 0;
  p.kind = v->strength > 0 ? v->kind : RNF_VTR_NONE;
  return p;
}

int rnf_vtr_active(const rnf_vtr* v) { return v && v->strength > 0 ? 1 : 0; }
void rnf_vtr_reset(rnf_vtr* v) {
  if (!v) return;
  v->strength = 0;
  v->last = -1;
  v->kind = RNF_VTR_NONE;
}

void rnf_vtr_apply(const uint32_t* in, uint32_t* out, const rnf_vtr_params* p) {
  if (!in || !out) return;
  float s = p ? std::min(1.0f, std::max(0.0f, p->strength)) : 0.0f;
  if (s <= 0) {
    std::copy(in, in + kPixels, out);
    return;
  }
  const bool ff = p->kind == RNF_VTR_FAST_FORWARD;
  const bool sweep = p->kind == RNF_VTR_RETURN;
  const double t = p->time;
  const uint32_t seed = uint32_t(int64_t(std::floor(t * 30.0)));  // the noise changes at 30 Hz

  // Look: everything scales with s. Luminance-neutral parts (desaturation, chroma bleed, jitter) are
  // the strongest; brightness changes stay small (lifted blacks / softer whites, washed bands).
  const float desat = ff ? 0.0f : 0.55f * s;
  const float contrast = 1.0f - (ff ? 0.0f : 0.05f) * s;
  const float chromaShift = ff ? 0.0f : 1.8f * s;
  const float jitterAmp = (ff ? 0.25f : 0.45f) * s;
  const float bandShift = (ff ? 2.0f : 4.0f) * s;
  const float bandLift = (ff ? 0.04f : 0.06f) * s;
  const float streakMix = (ff ? 0.22f : sweep ? 0.48f : 0.4f) * s;
  const float skewAmp = (ff ? 4.0f : 10.0f) * s;

  // Two soft tracking-noise bands drifting smoothly (up while rewinding, down for fast-forward).
  struct Band { double height, speed, phase; };
  const Band bands[2] = {
      {ff ? 5.0 : (sweep ? 17.0 : 13.0), ff ? 95.0 : (sweep ? -100.0 : -70.0), 40.0},
      {ff ? 4.0 : (sweep ? 11.0 : 9.0), ff ? 140.0 : (sweep ? -140.0 : -95.0), 170.0},
  };
  float bandAt[kH];
  for (int y = 0; y < kH; ++y) bandAt[y] = 0;
  for (const Band& b : bands) {
    double span = double(kH) + 2 * b.height;
    double c = std::fmod(b.phase + b.speed * t, span);
    if (c < 0) c += span;
    c -= b.height;
    int y0 = std::max(0, int(std::floor(c - b.height))), y1 = std::min(kH - 1, int(std::ceil(c + b.height)));
    for (int y = y0; y <= y1; ++y) {
      double d = (double(y) - c) / b.height;
      if (d <= -1 || d >= 1) continue;
      float w = float(0.5 * (1 + std::cos(3.14159265358979 * d)));
      bandAt[y] = std::max(bandAt[y], w);
    }
  }

  float rowR[kW], rowG[kW], rowB[kW], cr[kW], cg[kW], cb[kW];
  const float mid = 118.0f;
  for (int y = 0; y < kH; ++y) {
    const uint32_t* src = in + size_t(y) * kW;
    uint32_t* dst = out + size_t(y) * kW;
    float band = bandAt[y];
    // Horizontal displacement of this line: a gentle wobble, the band's tearing, the head-switching
    // skew of the last lines.
    float shift = jitterAmp * float(std::sin(y * 0.37 + t * 6.0) * std::sin(t * 3.1 + y * 0.05));
    // The band tears the lines smoothly (a slow ripple, no per-frame random jumps: no shimmer).
    if (band > 0) shift += band * bandShift * float(0.65 + 0.35 * std::sin(y * 0.8 + t * 9.0));
    if (y >= kH - 9) shift += skewAmp * float(y - (kH - 9)) / 8.0f;
    float sx0 = -shift;
    for (int x = 0; x < kW; ++x) {
      float fx = float(x) + sx0;
      int xi = int(std::floor(fx));
      float fr = fx - float(xi);
      int a = std::min(kW - 1, std::max(0, xi)), b = std::min(kW - 1, std::max(0, xi + 1));
      uint32_t pa = src[a], pb = src[b];
      float r = float((pa >> 16) & 0xFF) * (1 - fr) + float((pb >> 16) & 0xFF) * fr;
      float g = float((pa >> 8) & 0xFF) * (1 - fr) + float((pb >> 8) & 0xFF) * fr;
      float bl = float(pa & 0xFF) * (1 - fr) + float(pb & 0xFF) * fr;
      rowR[x] = r;
      rowG[x] = g;
      rowB[x] = bl;
      float l = 0.299f * r + 0.587f * g + 0.114f * bl;
      cr[x] = r - l;
      cg[x] = g - l;
      cb[x] = bl - l;
    }
    int ci = int(std::floor(chromaShift));
    float cf = chromaShift - float(ci);
    uint32_t lineSeed = hash3(uint32_t(y), seed, 3u);
    int segOffset = int(lineSeed & 7u);
    for (int x = 0; x < kW; ++x) {
      float l = 0.299f * rowR[x] + 0.587f * rowG[x] + 0.114f * rowB[x];
      int x1 = std::max(0, x - ci), x2 = std::max(0, x - ci - 1);
      float c0 = (1 - cf), c1 = cf;
      float ccr = cr[x1] * c0 + cr[x2] * c1, ccg = cg[x1] * c0 + cg[x2] * c1, ccb = cb[x1] * c0 + cb[x2] * c1;
      float lo = mid + (l - mid) * contrast;
      float keep = 1 - desat;
      if (band > 0) {
        lo += band * bandLift * (235.0f - lo);
        // Short horizontal streaks of tape noise (low contrast, inside the band only).
        uint32_t h = hash3(uint32_t((x + segOffset) >> 3), uint32_t(y), seed);
        float pick = unit(h);
        if (pick < 0.5f * band) {
          // Light and dark streaks alike: the band's mean brightness stays where it was.
          uint32_t h2 = h * 2654435761u;
          float m = streakMix * band * (0.6f + 0.4f * unit(h2));
          float toward = (h2 & 0x100u) ? 215.0f : 35.0f;
          lo = lo + (toward - lo) * m;
          keep *= 1 - m;
        }
      }
      float r = clamp255(lo + ccr * keep), g = clamp255(lo + ccg * keep), b = clamp255(lo + ccb * keep);
      dst[x] = (src[x] & 0xFF000000u) | (uint32_t(r + 0.5f) << 16) | (uint32_t(g + 0.5f) << 8) | uint32_t(b + 0.5f);
    }
  }
}

}  // extern "C"
