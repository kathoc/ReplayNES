// Pure timeline logic: frame <-> x mapping of the take timeline, A/B range gestures, take lineage
// (which frames two takes share), which A/B slots lie on the active take, the filmstrip grid at
// a fixed scale and the thumbnail downscaler.
//
// Filmstrip layout (video-editor style): one tile = F frames of game time = one picture at its
// natural width, F = 5 s * 2^k. The recorded part of the take occupies x = 0 ... length *
// tileWidth / F and grows to the right while recording. Tile k starts at x = k * tileWidth and
// shows the picture at frame k*F (tile 0: the screen 1 s in), clipped at the extent. F is the
// smallest step at which the whole take fits the strip.
// Thumbnail keys are CURSOR frames f (> 0): the picture shown at rn_frame() == f. It depends only
// on input records [0, f), so it is valid on every take that shares that prefix.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

#include "common.hpp"
#include "picture.hpp"
#include <limits>

using namespace rnf;

namespace {

double xFor(double width, uint64_t length, uint64_t f) {
  if (!(length > 0) || !(width > 0)) return 0;
  return double(std::min(f, length)) / double(length) * width;
}

uint64_t frameAt(double width, uint64_t length, double x) {
  if (!(length > 0) || !(width > 0)) return 0;
  double t = std::max(0.0, std::min(1.0, x / width));
  return std::min(length, uint64_t(std::round(t * double(length))));
}

double step(int e) { return std::ldexp(RNF_THUMB_BASE_STEP, std::max(RNF_THUMB_MIN_EXPONENT, std::min(RNF_THUMB_MAX_EXPONENT, e))); }
uint64_t gridFrame(uint64_t k, double f) { return uint64_t(std::round(double(k) * f)); }
uint64_t startPicture(double f) {
  return std::min<uint64_t>(RNF_THUMB_START_FRAME, std::max<uint64_t>(1, uint64_t(std::round(f / 2))));
}
uint64_t pictureFrame(uint64_t k, double f) { return k == 0 ? startPicture(f) : gridFrame(k, f); }
double extent(uint64_t length, double f, double tw) {
  if (!(f > 0) || !(tw > 0)) return 0;
  return double(length) / f * tw;
}

template <typename T>
size_t copyOut(const std::vector<T>& v, T* out, size_t cap) {
  if (out) for (size_t i = 0; i < v.size() && i < cap; ++i) out[i] = v[i];
  return v.size();
}

}  // namespace

namespace rnf {
bool isGridFrame(uint64_t x, double f) {
  if (x == 0 || !(f > 0)) return false;
  return gridFrame(uint64_t(std::round(double(x) / f)), f) == x;
}
uint64_t thumbStartPicture(double f) { return startPicture(f); }
}  // namespace rnf

extern "C" {

double rnf_timeline_x_for_frame(double width, uint64_t length, uint64_t frame) { return xFor(width, length, frame); }
uint64_t rnf_timeline_frame_at_x(double width, uint64_t length, double x) { return frameAt(width, length, x); }

int rnf_timeline_range_from_drag(double width, uint64_t length, double x0, double x1, uint64_t* a, uint64_t* b) {
  uint64_t f0 = frameAt(width, length, x0), f1 = frameAt(width, length, x1);
  uint64_t lo = std::min(f0, f1), hi = std::max(f0, f1);
  if (!(hi > lo)) return 0;
  if (a) *a = lo;
  if (b) *b = hi;
  return 1;
}

rnf_timeline_hit rnf_timeline_hit_test(double x, const rnf_timeline_range* ranges, size_t count, double width,
                                       uint64_t length, double tolerance, int has_preferred, int preferred) {
  rnf_timeline_hit none{RNF_HIT_NONE, 0, RNF_HANDLE_A};
  if (!ranges) return none;
  bool haveBest = false;
  double bestDist = 0;
  long bestRank = 0;
  rnf_timeline_hit best = none;
  auto rankOf = [&](size_t i) { return long((has_preferred && ranges[i].slot == preferred) ? 1000 : 0) + long(i); };
  for (size_t i = 0; i < count; ++i) {
    const rnf_timeline_range& r = ranges[i];
    long rank = rankOf(i);
    auto consider = [&](uint64_t edge, rnf_timeline_handle h) {
      double d = std::fabs(xFor(width, length, edge) - x);
      if (!(d <= tolerance)) return;
      if (haveBest && (bestDist < d || (bestDist == d && bestRank > rank))) return;
      haveBest = true;
      bestDist = d;
      bestRank = rank;
      best = rnf_timeline_hit{RNF_HIT_HANDLE, r.slot, h};
    };
    consider(r.a, RNF_HANDLE_A);
    if (r.has_b) consider(r.b, RNF_HANDLE_B);
  }
  if (haveBest) return best;
  bool haveBody = false;
  long bodyRank = 0;
  int bodySlot = 0;
  for (size_t i = 0; i < count; ++i) {
    const rnf_timeline_range& r = ranges[i];
    if (!r.has_b) continue;
    double x0 = xFor(width, length, r.a), x1 = xFor(width, length, r.b);
    if (!(x >= x0 && x <= x1)) continue;
    long rank = rankOf(i);
    if (!haveBody || rank > bodyRank) {
      haveBody = true;
      bodyRank = rank;
      bodySlot = r.slot;
    }
  }
  if (haveBody) return rnf_timeline_hit{RNF_HIT_BODY, bodySlot, RNF_HANDLE_A};
  return none;
}

void rnf_timeline_drag(rnf_timeline_handle handle, uint64_t a, uint64_t b, double x, double width, uint64_t length,
                       uint64_t* out_a, uint64_t* out_b) {
  uint64_t f = frameAt(width, length, x);
  uint64_t na = a, nb = b;
  if (handle == RNF_HANDLE_A) {
    uint64_t limit = b > 0 ? b - 1 : 0;
    na = std::min(f, limit);
  } else {
    nb = std::min(std::max(f, a + 1), std::max(length, a + 1));
  }
  if (out_a) *out_a = na;
  if (out_b) *out_b = nb;
}

rnf_mark_plan rnf_timeline_mark_a(uint64_t f, const rnf_timeline_range* r, uint64_t* a, uint64_t* b, char** message) {
  if (message) *message = nullptr;
  if (r && r->has_b && f < r->b) {
    if (a) *a = f;
    if (b) *b = r->b;
    return RNF_MARK_SET_RANGE;
  }
  return RNF_MARK_SET_A_ONLY;
}

rnf_mark_plan rnf_timeline_mark_b(uint64_t f, const rnf_timeline_range* r, uint64_t* a, uint64_t* b, char** message) {
  if (message) *message = nullptr;
  if (!r) {
    if (message) *message = dup(tr(RNF_L("Set A of this section first")));
    return RNF_MARK_INVALID;
  }
  if (!(f > r->a)) {
    if (message) *message = dup(tr(RNF_L("Set B at a position after A")));
    return RNF_MARK_INVALID;
  }
  if (a) *a = r->a;
  if (b) *b = f;
  return RNF_MARK_SET_RANGE;
}

uint64_t rnf_take_shared_prefix(const rn_take_info* takes, size_t count, uint64_t x, uint64_t y) {
  if (!takes) return 0;
  try {
    std::map<uint64_t, rn_take_info> byID;
    for (size_t i = 0; i < count; ++i) byID[takes[i].id] = takes[i];
    auto tx = byID.find(x);
    if (tx == byID.end() || byID.find(y) == byID.end()) return 0;
    if (x == y) return tx->second.length;
    auto chain = [&](uint64_t id) {  // id, parent, ..., root
      std::vector<uint64_t> out;
      uint64_t cur = id;
      while (cur != 0) {
        auto t = byID.find(cur);
        if (t == byID.end() || out.size() > byID.size()) break;
        out.push_back(cur);
        cur = t->second.parent_id;
      }
      return out;
    };
    std::vector<uint64_t> cx = chain(x), cy = chain(y);
    std::set<uint64_t> setX(cx.begin(), cx.end());
    size_t ci = 0;
    while (ci < cy.size() && !setX.count(cy[ci])) ++ci;
    if (ci == cy.size()) return 0;
    uint64_t common = cy[ci];
    size_t xi = size_t(std::find(cx.begin(), cx.end(), common) - cx.begin());
    // Frames of `common` used by each path end where that path's next segment branches off.
    uint64_t limX = xi == 0 ? byID[common].length : byID[cx[xi - 1]].branch_frame;
    uint64_t limY = ci == 0 ? byID[common].length : byID[cy[ci - 1]].branch_frame;
    return std::min(limX, limY);
  } catch (...) {
    return 0;
  }
}

size_t rnf_timeline_visible_ranges(const rnf_practice_slot* slots, size_t slot_count, const rn_take_info* takes,
                                   size_t take_count, uint64_t active_take, uint64_t take_length,
                                   rnf_timeline_range* out, size_t out_cap) {
  if (!slots) return 0;
  size_t n = 0;
  for (size_t i = 0; i < slot_count; ++i) {
    const rnf_practice_slot& s = slots[i];
    if (!s.has_a || !s.has_take_frame) continue;
    uint64_t end = s.has_b ? s.take_frame + s.length : s.take_frame;
    if (end > take_length || end > rnf_take_shared_prefix(takes, take_count, s.take_id, active_take)) continue;
    if (out && n < out_cap) out[n] = rnf_timeline_range{s.index, s.take_frame, s.has_b ? 1 : 0, s.has_b ? end : 0};
    ++n;
  }
  return n;
}

// ------------------------------------------------------------------ filmstrip grid
double rnf_thumb_step(int exponent) { return step(exponent); }

int rnf_thumb_is_step(double f) {
  if (!(f > 0) || !std::isfinite(f)) return 0;
  double l = std::round(std::log2(f / RNF_THUMB_BASE_STEP));
  if (l < RNF_THUMB_MIN_EXPONENT || l > RNF_THUMB_MAX_EXPONENT) return 0;
  return step(int(l)) == f;
}

uint64_t rnf_thumb_frame(uint64_t k, double f) { return gridFrame(k, f); }

size_t rnf_thumb_frames(uint64_t length, double f, uint64_t* out, size_t cap) {
  if (!(f > 0) || !(double(length) >= f)) return 0;
  uint64_t last = uint64_t(std::floor(double(length) / f));
  for (uint64_t k = 1; k <= last; ++k)
    if (out && k - 1 < cap) out[k - 1] = gridFrame(k, f);
  return size_t(last);
}

int rnf_thumb_is_grid_frame(uint64_t x, double f) { return isGridFrame(x, f); }
uint64_t rnf_thumb_start_picture(double f) { return startPicture(f); }
uint64_t rnf_thumb_picture_frame(uint64_t tile, double f) { return pictureFrame(tile, f); }

size_t rnf_thumb_targets(uint64_t length, double f, uint64_t* out, size_t cap) {
  if (length == 0 || !(f > 0)) return 0;
  try {
    uint64_t last = uint64_t(std::floor(double(length) / f));
    std::vector<uint64_t> v;
    for (uint64_t k = 0; k <= last; ++k) {
      uint64_t p = pictureFrame(k, f);
      if (p <= length && p > (v.empty() ? 0 : v.back())) v.push_back(p);
    }
    return copyOut(v, out, cap);
  } catch (...) {
    return 0;
  }
}

double rnf_thumb_extent(uint64_t length, double f, double tw) { return extent(length, f, tw); }

double rnf_thumb_tile_step(double width, double tw, uint64_t length) {
  double f = step(RNF_THUMB_MIN_EXPONENT);
  if (!(width > 0) || !(tw > 0)) return f;
  while (f < step(RNF_THUMB_MAX_EXPONENT) && extent(length, f, tw) > width + 1e-9) f *= 2;
  return f;
}

size_t rnf_thumb_tiles(uint64_t length, double f, double tw, rnf_thumb_tile* out, size_t cap) {
  if (length == 0 || !(f > 0) || !(tw > 0)) return 0;
  double end = extent(length, f, tw);
  size_t n = 0;
  for (uint64_t k = 0; double(k) * f < double(length); ++k) {
    double x = double(k) * tw;
    if (out && n < cap)
      out[n] = rnf_thumb_tile{k, gridFrame(k, f), pictureFrame(k, f), x, std::max(0.0, std::min(tw, end - x))};
    ++n;
  }
  return n;
}

double rnf_thumb_snap_to_pixel(double x, double scale) {
  if (!(scale > 0)) return x;
  return std::floor(x * scale + 1e-9) / scale;
}

uint64_t rnf_thumb_fallback_window(double f) { return uint64_t(std::ceil(f)); }

void rnf_thumb_downscale(const uint32_t* src, uint32_t* dst) {
  if (!src || !dst) return;
  const int sw = RN_VIDEO_WIDTH, k = RNF_THUMB_FACTOR;
  const uint32_t n = uint32_t(k * k);
  for (int y = 0; y < RNF_THUMB_HEIGHT; ++y) {
    for (int x = 0; x < RNF_THUMB_WIDTH; ++x) {
      uint32_t r = 0, g = 0, b = 0;
      for (int dy = 0; dy < k; ++dy) {
        const uint32_t* row = src + (y * k + dy) * sw + x * k;
        for (int dx = 0; dx < k; ++dx) {
          uint32_t p = row[dx];
          b += p & 0xFF;
          g += (p >> 8) & 0xFF;
          r += (p >> 16) & 0xFF;
        }
      }
      dst[y * RNF_THUMB_WIDTH + x] = 0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
    }
  }
}

int rnf_thumb_size(const rn_video_frame_info* frame, const rn_media_info* media,
                   uint32_t* width, uint32_t* height) {
  if (width) *width = 0;
  if (height) *height = 0;
  if (!frame || !media || !width || !height || !frame->width || !frame->height ||
      frame->width > media->max_width || frame->height > media->max_height ||
      !media->pixel_aspect_num || !media->pixel_aspect_den || media->rotation > 3) return 0;
  double aspect = double(frame->width) * media->pixel_aspect_num /
                  (double(frame->height) * media->pixel_aspect_den);
  if (media->rotation & 1) aspect = 1 / aspect;
  double h = std::max(1.0, std::round(RNF_THUMB_WIDTH / aspect));
  if (!std::isfinite(h) || h > std::numeric_limits<uint32_t>::max() ||
      h > double(SIZE_MAX / RNF_THUMB_WIDTH / sizeof(uint32_t))) return 0;
  *width = RNF_THUMB_WIDTH;
  *height = uint32_t(h);
  return 1;
}

int rnf_thumb_downscale_frame(const rn_video_frame_info* frame, const rn_media_info* media,
                              uint32_t* dst, size_t capacity) {
  uint32_t width = 0, height = 0;
  if (!rnf_thumb_size(frame, media, &width, &height) || !dst ||
      !rnf::validPicture(frame, media->max_width, media->max_height) ||
      capacity < size_t(width) * height) return 0;
  const bool quarter = (media->rotation & 1) != 0;
  const uint32_t sw = quarter ? frame->height : frame->width;
  const uint32_t sh = quarter ? frame->width : frame->height;
  // Box averaging in display coordinates; an exact 2:1 scale matches the NES downscaler.
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t y0 = uint32_t(uint64_t(y) * sh / height);
    const uint32_t y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * sh / height));
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t x0 = uint32_t(uint64_t(x) * sw / width);
      const uint32_t x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * sw / width));
      uint64_t r = 0, g = 0, b = 0;
      for (uint32_t sy = y0; sy < y1; ++sy) {
        for (uint32_t sx = x0; sx < x1; ++sx) {
          uint32_t ox = sx, oy = sy;
          switch (media->rotation) {
            case 1: ox = sy; oy = frame->height - 1 - sx; break;
            case 2: ox = frame->width - 1 - sx; oy = frame->height - 1 - sy; break;
            case 3: ox = frame->width - 1 - sy; oy = sx; break;
          }
          const uint32_t p = frame->pixels[size_t(oy) * frame->stride + ox];
          r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255;
        }
      }
      const uint64_t n = uint64_t(x1 - x0) * (y1 - y0);
      dst[size_t(y) * width + x] = 0xff000000u | uint32_t(r / n) << 16 |
                                  uint32_t(g / n) << 8 | uint32_t(b / n);
    }
  }
  return 1;
}

}  // extern "C"
