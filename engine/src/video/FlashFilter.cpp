// Photosensitive flash reduction - see FlashFilter.h and docs/FLASH_REDUCTION.md.
#include "video/FlashFilter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rn {
namespace {

constexpr int kOne = 65535;            // 1.0 in linear-light units
constexpr int kDarkLimit = 52428;      // WCAG: darker state below 0.80

struct Luts {
  uint16_t lin[256];
  uint8_t srgb[65536];
  Luts() {
    // IEC 61966-2-1 (the curve WCAG 2.x specifies; 0.04045 is the corrected breakpoint).
    for (int c = 0; c < 256; ++c) {
      double s = c / 255.0;
      double l = s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
      lin[c] = uint16_t(std::lround(l * kOne));
    }
    // Inverse by nearest code in linear light: thresholds at the midpoints between codes. Exact
    // integer comparisons, so lin -> srgb -> lin is the identity on every platform.
    int code = 0;
    for (int v = 0; v <= kOne; ++v) {
      while (code < 255 && 2 * v >= int(lin[code]) + int(lin[code + 1])) ++code;
      srgb[v] = uint8_t(code);
    }
  }
};

const Luts& luts() {
  static const Luts t;
  return t;
}

inline int chR(uint32_t p) { return int((p >> 16) & 0xFF); }
inline int chG(uint32_t p) { return int((p >> 8) & 0xFF); }
inline int chB(uint32_t p) { return int(p & 0xFF); }

}  // namespace

uint16_t FlashFilter::toLinear(uint8_t s) { return luts().lin[s]; }
uint8_t FlashFilter::toSrgb(uint16_t l) { return luts().srgb[l]; }

int FlashFilter::luminance(uint32_t px) {
  const Luts& t = luts();
  int r = t.lin[chR(px)], g = t.lin[chG(px)], b = t.lin[chB(px)];
  return (2126 * r + 7152 * g + 722 * b + 5000) / 10000;
}

int FlashFilter::redMetric(uint32_t px) {
  const Luts& t = luts();
  return std::max(0, int(t.lin[chR(px)]) - int(t.lin[chG(px)]) - int(t.lin[chB(px)]));
}

FlashParams FlashFilter::params(FlashLevel level) {
  FlashParams p;
  auto lin = [](double v) { return int(std::lround(v * kOne)); };
  auto red = [](double pts) { return int(std::lround(pts / 320.0 * kOne)); };  // WCAG (R-G-B)*320 scale
  switch (level) {
    case FlashLevel::Off:
      break;
    case FlashLevel::Low:  // WCAG 2.x thresholds as written
      p.lumThreshold = lin(0.10);
      p.lumRequireDark = true;
      p.redThreshold = red(20);
      p.redRequireSaturation = true;
      p.budget = 6;  // 3 flashes
      p.areaPermille = 250;
      p.lumHold = lin(0.06);
      p.lumRate = lin(0.02);
      p.redHold = red(12);
      p.redRate = red(4);
      p.stickyFrames = 8;
      break;
    case FlashLevel::Standard:  // stricter than WCAG: earlier detection, 2 flashes/s
      p.lumThreshold = lin(0.08);
      p.redThreshold = red(16);
      p.budget = 4;
      p.areaPermille = 200;
      p.lumHold = lin(0.04);
      p.lumRate = lin(0.01);
      p.redHold = red(10);
      p.redRate = red(2);
      p.stickyFrames = 12;
      break;
    case FlashLevel::High:  // 1 flash/s, smaller areas
      p.lumThreshold = lin(0.06);
      p.redThreshold = red(12);
      p.budget = 2;
      p.areaPermille = 150;
      p.lumHold = lin(0.02);
      p.lumRate = lin(0.005);
      p.redHold = red(6);
      p.redRate = red(1);
      p.stickyFrames = 20;
      break;
  }
  return p;
}

FlashFilter::FlashFilter(FlashLevel level)
    : level_(level), p_(params(level)), prev_(size_t(kWidth) * kHeight, 0xFF000000u), ref_(prev_) {
  luts();
}

void FlashFilter::setLevel(FlashLevel level) {
  level_ = level;
  p_ = params(level);
  reset();
}

void FlashFilter::reset() {
  primed_ = false;
  frame_ = 0;
  blocks_.fill(Block{});
}

FlashFilter::Stats FlashFilter::blockStats(const uint32_t* frame, int bx, int by) {
  const Luts& t = luts();
  int64_t sl = 0, sm = 0, sr = 0, sg = 0, sb = 0;
  for (int y = 0; y < kBlock; ++y) {
    const uint32_t* row = frame + size_t(by * kBlock + y) * kWidth + size_t(bx) * kBlock;
    for (int x = 0; x < kBlock; ++x) {
      uint32_t px = row[x];
      int r = t.lin[chR(px)], g = t.lin[chG(px)], b = t.lin[chB(px)];
      sl += (2126 * r + 7152 * g + 722 * b + 5000) / 10000;
      sm += std::max(0, r - g - b);
      sr += r;
      sg += g;
      sb += b;
    }
  }
  constexpr int n = kBlock * kBlock;
  Stats s;
  s.lum = int(sl / n);
  s.red = int(sm / n);
  s.sat = sr > 0 && 5 * sr >= 4 * (sr + sg + sb);
  return s;
}

FlashFilter::Stats FlashFilter::blendBlock(const uint32_t* in, uint32_t* out, int bx, int by, int a) const {
  const Luts& t = luts();
  const int na = 256 - a;
  for (int y = 0; y < kBlock; ++y) {
    size_t off = size_t(by * kBlock + y) * kWidth + size_t(bx) * kBlock;
    const uint32_t* ri = in + off;
    const uint32_t* rp = prev_.data() + off;
    uint32_t* ro = out + size_t(y) * kBlock;
    for (int x = 0; x < kBlock; ++x) {
      uint32_t pi = ri[x], pp = rp[x];
      auto mix = [&](int ci, int cp) { return int(t.srgb[(int(t.lin[cp]) * na + int(t.lin[ci]) * a + 128) >> 8]); };
      int r = mix(chR(pi), chR(pp)), g = mix(chG(pi), chG(pp)), b = mix(chB(pi), chB(pp));
      ro[x] = 0xFF000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
    }
  }
  // Stats of the quantized result (what will actually be shown). `out` is a packed 16x16 block.
  int64_t sl = 0, sm = 0, sr = 0, sg = 0, sb = 0;
  for (int i = 0; i < kBlock * kBlock; ++i) {
    uint32_t px = out[i];
    int r = t.lin[chR(px)], g = t.lin[chG(px)], b = t.lin[chB(px)];
    sl += (2126 * r + 7152 * g + 722 * b + 5000) / 10000;
    sm += std::max(0, r - g - b);
    sr += r;
    sg += g;
    sb += b;
  }
  constexpr int n = kBlock * kBlock;
  Stats s;
  s.lum = int(sl / n);
  s.red = int(sm / n);
  s.sat = sr > 0 && 5 * sr >= 4 * (sr + sg + sb);
  return s;
}

void FlashFilter::copyBlock(const uint32_t* src, uint32_t* dst, int bx, int by) const {
  for (int y = 0; y < kBlock; ++y) {
    size_t off = size_t(by * kBlock + y) * kWidth + size_t(bx) * kBlock;
    std::memcpy(dst + off, src + off, kBlock * sizeof(uint32_t));
  }
}

int FlashFilter::changedPixels(const uint32_t* in, const uint32_t* ref, int bx, int by) const {
  int n = 0;
  for (int y = 0; y < kBlock; ++y) {
    size_t off = size_t(by * kBlock + y) * kWidth + size_t(bx) * kBlock;
    for (int x = 0; x < kBlock; ++x) {
      uint32_t a = in[off + size_t(x)], b = ref[off + size_t(x)];
      if (a == b) continue;
      if (std::abs(luminance(a) - luminance(b)) >= p_.lumThreshold || std::abs(redMetric(a) - redMetric(b)) >= p_.redThreshold) ++n;
    }
  }
  return n;
}

int FlashFilter::classify(const Tracker& t, int v, bool sat, bool isRed) const {
  const int thr = isRed ? p_.redThreshold : p_.lumThreshold;
  auto cond = [&](int darker, bool satA) {
    if (isRed) return !p_.redRequireSaturation || satA || sat;
    return !p_.lumRequireDark || darker < kDarkLimit;
  };
  if (t.dir == 0) {
    if (v - t.lo >= thr && cond(t.lo, t.loSat)) return +1;
    if (t.hi - v >= thr && cond(v, t.hiSat)) return -1;
    return 0;
  }
  if (t.dir > 0) return (t.ext - v >= thr && cond(v, t.extSat)) ? -1 : 0;
  return (v - t.ext >= thr && cond(t.ext, t.extSat)) ? +1 : 0;
}

bool FlashFilter::update(Tracker& t, int v, bool sat, bool isRed) const {
  int c = classify(t, v, sat, isRed);
  if (c != 0) {
    t.dir = c;
    t.ext = v;
    t.extSat = sat;
    return true;
  }
  if (t.dir == 0) {
    if (v < t.lo) t.lo = v, t.loSat = sat;
    if (v > t.hi) t.hi = v, t.hiSat = sat;
  } else if ((t.dir > 0 && v > t.ext) || (t.dir < 0 && v < t.ext)) {
    t.ext = v;
    t.extSat = sat;
  }
  return false;
}

int FlashFilter::opposingDistance(const Tracker& t, int v) const {
  if (t.dir > 0) return std::max(0, t.ext - v);
  if (t.dir < 0) return std::max(0, v - t.ext);
  return std::max({0, v - t.lo, t.hi - v});
}

int FlashFilter::transitionsInWindow(const Block& b) const {
  int n = 0;
  for (int i = 0; i < b.histCount; ++i)
    if (b.hist[size_t(i)] > frame_ - kWindow) ++n;
  return n;
}

FlashFrameInfo FlashFilter::process(const uint32_t* in, uint32_t* out) {
  FlashFrameInfo info;
  const size_t npx = size_t(kWidth) * kHeight;
  if (level_ == FlashLevel::Off) {
    if (in != out) std::memcpy(out, in, npx * sizeof(uint32_t));
    return info;
  }

  std::array<Stats, kBlocks> inS;
  for (int by = 0; by < kRows; ++by)
    for (int bx = 0; bx < kCols; ++bx) inS[size_t(by * kCols + bx)] = blockStats(in, bx, by);

  if (!primed_) {
    // First frame after a reset: shown as is; it is the reference for the trackers.
    if (in != out) std::memcpy(out, in, npx * sizeof(uint32_t));
    std::memcpy(prev_.data(), out, npx * sizeof(uint32_t));
    std::memcpy(ref_.data(), out, npx * sizeof(uint32_t));
    for (int i = 0; i < kBlocks; ++i) {
      Block& b = blocks_[size_t(i)];
      const Stats& s = inS[size_t(i)];
      b = Block{};
      b.lum.ext = b.lum.lo = b.lum.hi = s.lum;
      b.red.ext = b.red.lo = b.red.hi = s.red;
      b.lum.extSat = b.lum.loSat = b.lum.hiSat = s.sat;
      b.red.extSat = b.red.loSat = b.red.hiSat = s.sat;
      b.shown = s;
    }
    primed_ = true;
    frame_ = 0;
    return info;
  }
  ++frame_;
  const int64_t f = frame_;

  // 1. Which blocks would make a transition if the new frame were shown unchanged?
  // The area of a flash is measured in pixels that really change (a block only partly covered by
  // a blinking object counts only that part), summed per direction over the new transitions and
  // those of the last kRecent-1 frames.
  std::array<int, kBlocks> want{}, changed{};
  int64_t up = 0, down = 0;
  for (int by = 0; by < kRows; ++by) {
    for (int bx = 0; bx < kCols; ++bx) {
      const size_t i = size_t(by * kCols + bx);
      const Block& b = blocks_[i];
      const Stats& s = inS[i];
      int cl = classify(b.lum, s.lum, s.sat, false);
      int cr = classify(b.red, s.red, s.sat, true);
      want[i] = cl != 0 ? cl : cr;
      int d = want[i], px = 0;
      if (d != 0) {
        // Against the previous frame (sudden flash) and against the extremum (gradual flash).
        px = changed[i] = std::max(changedPixels(in, prev_.data(), bx, by), changedPixels(in, ref_.data(), bx, by));
      } else if (f - b.lastTransition < kRecent) {  // part of the same flash
        d = b.lastDir;
        px = b.lastChanged;
      }
      if (d > 0) up += px;
      if (d < 0) down += px;
    }
  }
  bool anyWant = std::any_of(want.begin(), want.end(), [](int w) { return w != 0; });
  info.eventAreaPermille = int(std::max(up, down) * 1000 / (int64_t(kWidth) * kHeight));
  info.largeArea = anyWant && info.eventAreaPermille >= p_.areaPermille;

  // 2. Budget check for a large-area event. Keep one transition in reserve for going darker, so
  //    a suppressed screen rests on the darker state.
  std::array<char, kBlocks> held{};
  if (info.largeArea) {
    int64_t heldPx = 0, allowedPx = 0;
    int exhausted = 0, heldOnly = 0;
    for (int i = 0; i < kBlocks; ++i) {
      int n = transitionsInWindow(blocks_[size_t(i)]);
      if (n >= p_.budget) ++exhausted;
      if (want[size_t(i)] == 0) continue;
      const Block& b = blocks_[size_t(i)];
      int remaining = p_.budget - n;
      // The same swing seen by the other metric (e.g. luminance then red rising one frame later)
      // is not a new transition; otherwise one unit per transition, plus the darker reserve.
      bool ok = (b.histCount > 0 && want[size_t(i)] == b.lastDir) ||
                (want[size_t(i)] > 0 && p_.budget >= 4 ? remaining >= 2 : remaining >= 1);
      held[size_t(i)] = !ok;
      (ok ? allowedPx : heldPx) += changed[size_t(i)];
      if (!ok && n < p_.budget) ++heldOnly;
    }
    // A few blocks out of budget inside a change the rest of the screen is allowed to make (e.g.
    // a scene cut right after a title animation) would leave stale patches behind. Let them follow
    // the majority as long as the blocks over budget stay a small part of the screen (< 20%, below
    // the large-area threshold of every level), so no large area can exceed the budget.
    if (heldPx > 0 && heldPx * 1000 <= int64_t(kWidth) * kHeight * 100 && allowedPx >= 4 * heldPx &&
        (exhausted + heldOnly) * 5 < kBlocks)
      held.fill(0);
  }

  // 3. Per block: pass through, or hold / smooth by blending with the previous output.
  uint32_t tmp[kBlock * kBlock];
  for (int by = 0; by < kRows; ++by) {
    for (int bx = 0; bx < kCols; ++bx) {
      const size_t i = size_t(by * kCols + bx);
      Block& b = blocks_[i];
      const Stats& s = inS[i];
      enum { Pass, Hold, Smooth } mode = Pass;
      if (want[i] != 0) {
        if (held[i]) mode = Hold;
      } else if (f < b.stickyUntil) {
        mode = Smooth;
      }

      Stats shown = s;
      int a = 256;
      if (mode != Pass) {
        const Stats prevShown = b.shown;
        auto ok = [&](const Stats& c) {
          if (classify(b.lum, c.lum, c.sat, false) != 0 || classify(b.red, c.red, c.sat, true) != 0) return false;
          if (std::abs(c.lum - prevShown.lum) > p_.lumRate || std::abs(c.red - prevShown.red) > p_.redRate) return false;
          if (mode == Hold) {
            if (opposingDistance(b.lum, c.lum) > std::max(p_.lumHold, opposingDistance(b.lum, prevShown.lum))) return false;
            if (opposingDistance(b.red, c.red) > std::max(p_.redHold, opposingDistance(b.red, prevShown.red))) return false;
          }
          return true;
        };
        if (!(mode == Smooth && ok(s))) {
          int lo = 0, hi = 256;  // invariant: ok(lo) (lo = previous output), !ok(hi)
          while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            if (ok(blendBlock(in, tmp, bx, by, mid))) lo = mid;
            else hi = mid;
          }
          a = lo;
          shown = blendBlock(in, tmp, bx, by, a);
        }
        if (mode == Hold) b.stickyUntil = f + p_.stickyFrames;
      }

      if (a == 256) {
        if (in != out)
          for (int y = 0; y < kBlock; ++y) {
            size_t off = size_t(by * kBlock + y) * kWidth + size_t(bx) * kBlock;
            std::memcpy(out + off, in + off, kBlock * sizeof(uint32_t));
          }
      } else {
        for (int y = 0; y < kBlock; ++y) {
          size_t off = size_t(by * kBlock + y) * kWidth + size_t(bx) * kBlock;
          std::memcpy(out + off, tmp + size_t(y) * kBlock, kBlock * sizeof(uint32_t));
        }
        ++info.alteredBlocks;
      }

      const Tracker before = b.lum;
      bool tl = update(b.lum, shown.lum, shown.sat, false);
      if (b.lum.ext != before.ext || b.lum.lo != before.lo || b.lum.hi != before.hi || b.lum.dir != before.dir)
        copyBlock(out, ref_.data(), bx, by);
      int lumDir = b.lum.dir;
      bool tr = update(b.red, shown.red, shown.sat, true);
      if (tl || tr) {
        int dir = tl ? lumDir : b.red.dir;
        // Only a change of direction is a new transition of the block (a flash = two of them);
        // the other metric confirming the same swing is not counted twice.
        if (b.histCount == 0 || dir != b.lastDir) {
          b.hist[size_t(b.histHead)] = f;
          b.histHead = (b.histHead + 1) % kHistory;
          b.histCount = std::min(b.histCount + 1, kHistory);
        }
        b.lastTransition = f;
        b.lastDir = dir;
        b.lastChanged = changed[i] != 0 ? changed[i] : kBlock * kBlock;
      }
      b.shown = shown;
    }
  }
  std::memcpy(prev_.data(), out, npx * sizeof(uint32_t));
  info.altered = info.alteredBlocks > 0;
  return info;
}

}  // namespace rn
