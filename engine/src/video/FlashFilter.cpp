// Photosensitive flash reduction - see FlashFilter.h and docs/FLASH_REDUCTION.md.
#include "video/FlashFilter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(RN_FLASH_NO_SIMD)  // portable scalar code only (tests)
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define RN_FLASH_SSE2 1
#elif defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#define RN_FLASH_NEON 1
#endif

namespace rn {
namespace {

constexpr int kOne = 65535;            // 1.0 in linear-light units
constexpr int kDarkLimit = 52428;      // WCAG: darker state below 0.80

struct Luts {
  uint16_t lin[256];
  uint8_t srgb[65536];
  uint16_t q[65536];  // lin[srgb[v]]: linear light of the quantised code (stats of a blend)
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
      q[v] = lin[code];
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

// Per-pixel WCAG luminance exactly as luminance(): floor((2126 r + 7152 g + 722 b + 5000) / 10000)
// (the operands are non-negative and < 2^31, so unsigned division gives the same result).
inline uint32_t lumOf(uint32_t r, uint32_t g, uint32_t b) { return (2126 * r + 7152 * g + 722 * b + 5000) / 10000; }
inline uint32_t redOf(int r, int g, int b) { return uint32_t(std::max(0, r - g - b)); }

inline void prefetchRange(const void* p, size_t bytes) {
#if defined(__GNUC__) || defined(__clang__)
  const char* c = static_cast<const char*>(p);
  for (size_t o = 0; o < bytes; o += 64) __builtin_prefetch(c + o);
#else
  (void)p;
  (void)bytes;
#endif
}

// Number of the n entries of v equal to x (n: multiple of 8, <= 65535).
inline int countEqual(const uint16_t* v, int n, uint16_t x) {
#if defined(RN_FLASH_SSE2)
  const __m128i vx = _mm_set1_epi16(short(x));
  __m128i acc = _mm_setzero_si128();
  for (int k = 0; k < n; k += 8)
    acc = _mm_sub_epi16(acc, _mm_cmpeq_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(v + k)), vx));
  acc = _mm_madd_epi16(acc, _mm_set1_epi16(1));  // 4 x int32 (lanes <= n / 8 each)
  acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, 0x4E));
  acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, 0xB1));
  return _mm_cvtsi128_si32(acc);
#elif defined(RN_FLASH_NEON)
  const uint16x8_t vx = vdupq_n_u16(x);
  uint16x8_t acc = vdupq_n_u16(0);
  for (int k = 0; k < n; k += 8) acc = vsubq_u16(acc, vceqq_u16(vld1q_u16(v + k), vx));
  return int(vaddlvq_u16(acc));
#else
  int c = 0;
  for (int k = 0; k < n; ++k) c += v[k] == x;
  return c;
#endif
}

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
    : level_(level),
      p_(params(level)),
      prev_(size_t(kWidth) * kHeight, 0xFF000000u),
      // Black has luminance 0 and red metric 0: the caches of prev_ (and the reference) start in sync.
      inM_(size_t(kWidth) * kHeight, 0),
      prevM_(inM_),
      refM_(inM_),
      pairCache_(size_t(1) << kPairCacheBits),
      blend_(1) {
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

FlashFilter::PairEntry FlashFilter::measurePair(const uint32_t* px, uint64_t key) {
  const Luts& t = luts();
  PairEntry e;
  e.key = key;
  e.s0 = e.s1 = 0;
  for (int k = 0; k < 2; ++k) {
    const uint32_t r = t.lin[chR(px[k])], g = t.lin[chG(px[k])], b = t.lin[chB(px[k])];
    const uint32_t l = lumOf(r, g, b), m = redOf(int(r), int(g), int(b));
    e.lr[k] = l | m << 16;
    e.s0 += l | uint64_t(m) << 32;
    e.s1 += r | uint64_t(r + g + b) << 32;
  }
  return e;
}

void FlashFilter::measureBlock(const uint32_t* in, int bx, int by) {
  const size_t i = size_t(by * kCols + bx);
  const size_t off0 = size_t(by * kBlock) * kWidth + size_t(bx) * kBlock;
  const uint32_t* src = in + off0;
  if (prevIsLastIn_[i]) {
    // The displayed block is the last input block, which inS_[i] / inM_ describe: reuse them if
    // the new input block is the same.
    const uint32_t* prev = prev_.data() + off0;
    bool same = true;
    for (int y = 0; y < kBlock && same; ++y)
      same = std::memcmp(src + size_t(y) * kWidth, prev + size_t(y) * kWidth, kBlock * sizeof(uint32_t)) == 0;
    if (same) return;
  }
  PairEntry* cache = pairCache_.data();
  uint32_t* M = inM_.data() + i * kBlockPx;
  // Packed sums: s0 = sum(lum) | sum(red) << 32, s1 = sum(R) | sum(R+G+B) << 32 (each < 2^26).
  uint64_t s0 = 0, s1 = 0;
  auto pairKey = [](const uint32_t* px) {  // both pixels (only compared and hashed)
    uint64_t key;
    std::memcpy(&key, px, sizeof(key));
    return key;
  };
  if (directBlocks_ > 0) {  // many distinct colours (e.g. noise): the cache would thrash
    --directBlocks_;
    for (int y = 0; y < kBlock; ++y) {
      const uint32_t* row = src + size_t(y) * kWidth;
      for (int x = 0; x < kBlock; x += 2) {
        const PairEntry e = measurePair(row + x, pairKey(row + x));
        std::memcpy(M + y * kBlock + x, e.lr, sizeof(e.lr));
        s0 += e.s0;
        s1 += e.s1;
      }
    }
  } else {
    int misses = 0;
    for (int y = 0; y < kBlock; ++y) {
      const uint32_t* row = src + size_t(y) * kWidth;
      for (int x = 0; x < kBlock; x += 2) {
        const uint64_t key = pairKey(row + x);
        PairEntry& e = cache[(key * 0x9E3779B97F4A7C15ull) >> (64 - kPairCacheBits)];
        if (e.key != key) {
          e = measurePair(row + x, key);
          ++misses;
        }
        std::memcpy(M + y * kBlock + x, e.lr, sizeof(e.lr));
        s0 += e.s0;
        s1 += e.s1;
      }
    }
    if (misses > kBlockPx / 8) directBlocks_ = 8;
  }
  const uint32_t sl = uint32_t(s0), sm = uint32_t(s0 >> 32), sr = uint32_t(s1), srgb = uint32_t(s1 >> 32);
  Stats& st = inS_[i];
  st.lum = int(sl / kBlockPx);
  st.red = int(sm / kBlockPx);
  st.sat = sr > 0 && 5 * int64_t(sr) >= 4 * int64_t(srgb);
}

void FlashFilter::prepareBlend(const uint32_t* in, int bx, int by) {
  const Luts& t = luts();
  BlendScratch& s = blend_[0];
  // Hash slot tag = generation << 9 | index; a new generation per block empties both tables.
  if (++s.gen >= (1u << 23)) {
    std::memset(s.tag, 0, sizeof(s.tag));
    std::memset(s.ptag, 0, sizeof(s.ptag));
    s.gen = 1;
  }
  if (identityBlocks_ > 0) {
    // Many distinct pixels (e.g. noise): deduplication would not pay off. One unit per pixel gives
    // the same integer sums.
    --identityBlocks_;
    for (int y = 0; y < kBlock; ++y) {
      const size_t off = size_t(by * kBlock + y) * kWidth + size_t(bx) * kBlock;
      for (int x = 0; x < kBlock; ++x) {
        const int u = y * kBlock + x;
        const uint32_t pp = prev_[off + size_t(x)], pi = in[off + size_t(x)];
        const int lp[3] = {t.lin[chR(pp)], t.lin[chG(pp)], t.lin[chB(pp)]};
        const int li[3] = {t.lin[chR(pi)], t.lin[chG(pi)], t.lin[chB(pi)]};
        for (int c = 0; c < 3; ++c) {
          s.base[c][u] = lp[c] * 256 + 128;
          s.diff[c][u] = li[c] - lp[c];
        }
        s.count[u] = 1;
      }
    }
    for (int q = 0; q < kBlockPx / 2; ++q) {
      s.pairUnit[q][0] = uint16_t(2 * q);
      s.pairUnit[q][1] = uint16_t(2 * q + 1);
      s.pairOf[q] = uint16_t(q);
    }
    s.units = kBlockPx;
    s.pairs = kBlockPx / 2;
    return;
  }
  const uint32_t gen = s.gen;
  int units = 0, pairs = 0;
  // Unit of one (prev, in) pixel value pair: only called when a new pixel-pair unit appears.
  auto findUnit = [&](uint32_t pp, uint32_t pi) {
    const uint64_t key = uint64_t(pp) << 32 | pi;
    uint32_t h = uint32_t((key * 0x9E3779B97F4A7C15ull) >> 55);  // 512 slots >= 2 * 256 units
    uint32_t tg = s.tag[h];
    while ((tg >> 9) == gen && s.key[h] != key) {
      h = (h + 1) & (2 * kBlockPx - 1);
      tg = s.tag[h];
    }
    if ((tg >> 9) != gen) {
      tg = gen << 9 | uint32_t(units);
      s.tag[h] = tg;
      s.key[h] = key;
      const int lp[3] = {t.lin[chR(pp)], t.lin[chG(pp)], t.lin[chB(pp)]};
      const int li[3] = {t.lin[chR(pi)], t.lin[chG(pi)], t.lin[chB(pi)]};
      for (int c = 0; c < 3; ++c) {
        s.base[c][units] = lp[c] * 256 + 128;
        s.diff[c][units] = li[c] - lp[c];
      }
      ++units;
    }
    return uint16_t(tg & 511);
  };
  // Horizontally adjacent pixels are deduplicated as pairs (half the hashing; writeBlend then
  // writes two pixels at a time).
  for (int y = 0; y < kBlock; ++y) {
    const size_t off = size_t(by * kBlock + y) * kWidth + size_t(bx) * kBlock;
    const uint32_t* ri = in + off;
    const uint32_t* rp = prev_.data() + off;
    for (int x = 0; x < kBlock; x += 2) {
      uint64_t kp, ki;  // the two prev / in pixels (memory order; only compared and hashed)
      std::memcpy(&kp, rp + x, sizeof(kp));
      std::memcpy(&ki, ri + x, sizeof(ki));
      uint32_t h = uint32_t(((kp * 0x9E3779B97F4A7C15ull) ^ (ki * 0xC2B2AE3D27D4EB4Full)) >> 56);  // 256 slots
      uint32_t tg = s.ptag[h];
      while ((tg >> 9) == gen && (s.pkey[h][0] != kp || s.pkey[h][1] != ki)) {
        h = (h + 1) & (kBlockPx - 1);
        tg = s.ptag[h];
      }
      if ((tg >> 9) != gen) {
        tg = gen << 9 | uint32_t(pairs);
        s.ptag[h] = tg;
        s.pkey[h][0] = kp;
        s.pkey[h][1] = ki;
        s.pairUnit[pairs][0] = findUnit(rp[x], ri[x]);
        s.pairUnit[pairs][1] = findUnit(rp[x + 1], ri[x + 1]);
        ++pairs;
      }
      s.pairOf[y * (kBlock / 2) + x / 2] = uint16_t(tg & 511);
    }
  }
  s.units = units;
  s.pairs = pairs;
  if (units > kBlockPx / 2) identityBlocks_ = 8;
  // Pixel counts per unit, via the counts per pixel-pair unit. Few pair units: branch-free
  // compare-and-count (SIMD); otherwise 4 interleaved histograms (no store-to-load chain on runs).
  constexpr int kPairs = kBlockPx / 2;
  int32_t pc[kPairs];
  if (pairs <= 12) {
    for (int q = 0; q < pairs; ++q) pc[q] = countEqual(s.pairOf, kPairs, uint16_t(q));
  } else {
    int32_t hist[4][kPairs];
    for (int k = 0; k < 4; ++k) std::memset(hist[k], 0, size_t(pairs) * sizeof(int32_t));
    for (int k = 0; k < kPairs; k += 4) {
      ++hist[0][s.pairOf[k]];
      ++hist[1][s.pairOf[k + 1]];
      ++hist[2][s.pairOf[k + 2]];
      ++hist[3][s.pairOf[k + 3]];
    }
    for (int q = 0; q < pairs; ++q) pc[q] = hist[0][q] + hist[1][q] + hist[2][q] + hist[3][q];
  }
  std::memset(s.count, 0, size_t(units) * sizeof(int32_t));
  for (int q = 0; q < pairs; ++q) {
    s.count[s.pairUnit[q][0]] += pc[q];
    s.count[s.pairUnit[q][1]] += pc[q];
  }
}

// Stats of the quantised blend, exactly as computed per pixel on the written block: every pixel of a
// unit has the same value, so the integer sums are count * value.
FlashFilter::Stats FlashFilter::blendStats(int a) const {
  const Luts& t = luts();
  const BlendScratch& s = blend_[0];
  // Sums stay below 2^31: <= 256 * 65535 (sat test: 4 * 3 * 256 * 65535).
  int32_t sl = 0, sm = 0, sr = 0, sg = 0, sb = 0;
  for (int u = 0; u < s.units; ++u) {
    const int r = t.q[(s.base[0][u] + s.diff[0][u] * a) >> 8];
    const int g = t.q[(s.base[1][u] + s.diff[1][u] * a) >> 8];
    const int b = t.q[(s.base[2][u] + s.diff[2][u] * a) >> 8];
    const int32_t c = s.count[u];
    sl += c * int32_t(lumOf(uint32_t(r), uint32_t(g), uint32_t(b)));
    sm += c * int32_t(redOf(r, g, b));
    sr += c * r;
    sg += c * g;
    sb += c * b;
  }
  Stats st;
  st.lum = sl / kBlockPx;
  st.red = sm / kBlockPx;
  st.sat = sr > 0 && 5 * sr >= 4 * (sr + sg + sb);
  return st;
}

FlashFilter::Stats FlashFilter::writeBlend(uint32_t* out, int bx, int by, int a) {
  const Luts& t = luts();
  const BlendScratch& s = blend_[0];
  uint32_t color[kBlockPx], lr[kBlockPx];
  int64_t sl = 0, sm = 0, sr = 0, sg = 0, sb = 0;
  for (int u = 0; u < s.units; ++u) {
    const int cr = t.srgb[(s.base[0][u] + s.diff[0][u] * a) >> 8];
    const int cg = t.srgb[(s.base[1][u] + s.diff[1][u] * a) >> 8];
    const int cb = t.srgb[(s.base[2][u] + s.diff[2][u] * a) >> 8];
    color[u] = 0xFF000000u | uint32_t(cr) << 16 | uint32_t(cg) << 8 | uint32_t(cb);
    const int r = t.lin[cr], g = t.lin[cg], b = t.lin[cb];
    const uint32_t l = lumOf(uint32_t(r), uint32_t(g), uint32_t(b)), m = redOf(r, g, b);
    lr[u] = l | m << 16;
    const int64_t c = s.count[u];
    sl += c * l;
    sm += c * m;
    sr += c * r;
    sg += c * g;
    sb += c * b;
  }
  const size_t i = size_t(by * kCols + bx);
  // Per pixel-pair unit: both output pixels and both metrics, in memory order.
  uint32_t color2[kBlockPx / 2][2], lr2[kBlockPx / 2][2];
  for (int q = 0; q < s.pairs; ++q) {
    const int u0 = s.pairUnit[q][0], u1 = s.pairUnit[q][1];
    color2[q][0] = color[u0];
    color2[q][1] = color[u1];
    lr2[q][0] = lr[u0];
    lr2[q][1] = lr[u1];
  }
  uint32_t* M = prevM_.data() + i * kBlockPx;
  for (int y = 0; y < kBlock; ++y) {
    uint32_t* ro = out + size_t(by * kBlock + y) * kWidth + size_t(bx) * kBlock;
    for (int x = 0; x < kBlock; x += 2) {
      const int q = s.pairOf[y * (kBlock / 2) + x / 2];
      std::memcpy(ro + x, color2[q], 2 * sizeof(uint32_t));
      std::memcpy(M + y * kBlock + x, lr2[q], 2 * sizeof(uint32_t));
    }
  }
  Stats st;
  st.lum = int(sl / kBlockPx);
  st.red = int(sm / kBlockPx);
  st.sat = sr > 0 && 5 * sr >= 4 * (sr + sg + sb);
  return st;
}

// Equivalent to counting, per pixel that differs, |dL| >= lumThreshold || |dM| >= redThreshold:
// identical pixels have equal metrics and both thresholds are > 0 whenever the filter is on.
void FlashFilter::changedPixels(size_t i, int& vsPrev, int& vsRef) const {
  const size_t o = i * kBlockPx;
  const uint32_t *im = inM_.data() + o, *pm = prevM_.data() + o, *rm = refM_.data() + o;
  const int tl = p_.lumThreshold, tr = p_.redThreshold;  // both in 1..65535
#if defined(RN_FLASH_SSE2) || defined(RN_FLASH_NEON)
  // 16-bit lanes alternate luminance / red metric; a pixel changed if either lane's absolute
  // difference reaches its threshold.
  const uint32_t thr = uint32_t(tl) | uint32_t(tr) << 16;
#endif
#if defined(RN_FLASH_SSE2)
  const __m128i vt = _mm_set1_epi32(int(thr)), zero = _mm_setzero_si128();
  __m128i unchP = zero, unchR = zero;  // -1 per unchanged pixel
  for (int k = 0; k < kBlockPx; k += 4) {
    const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(im + k));
    const __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pm + k));
    const __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rm + k));
    const __m128i dp = _mm_or_si128(_mm_subs_epu16(a, p), _mm_subs_epu16(p, a));
    const __m128i dr = _mm_or_si128(_mm_subs_epu16(a, r), _mm_subs_epu16(r, a));
    // lane below threshold <=> thr -sat d > 0; pixel unchanged <=> both lanes below.
    const __m128i bp = _mm_cmpeq_epi16(_mm_subs_epu16(vt, dp), zero);  // 0xFFFF where d >= thr
    const __m128i br = _mm_cmpeq_epi16(_mm_subs_epu16(vt, dr), zero);
    unchP = _mm_add_epi32(unchP, _mm_cmpeq_epi32(bp, zero));
    unchR = _mm_add_epi32(unchR, _mm_cmpeq_epi32(br, zero));
  }
  auto hsum = [](__m128i v) {
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0x4E));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, 0xB1));
    return _mm_cvtsi128_si32(v);
  };
  vsPrev = kBlockPx + hsum(unchP);
  vsRef = kBlockPx + hsum(unchR);
#elif defined(RN_FLASH_NEON)
  const uint16x8_t vt = vreinterpretq_u16_u32(vdupq_n_u32(thr));
  uint32x4_t chP = vdupq_n_u32(0), chR = vdupq_n_u32(0);
  for (int k = 0; k < kBlockPx; k += 4) {
    const uint16x8_t a = vreinterpretq_u16_u32(vld1q_u32(im + k));
    const uint16x8_t p = vreinterpretq_u16_u32(vld1q_u32(pm + k));
    const uint16x8_t r = vreinterpretq_u16_u32(vld1q_u32(rm + k));
    const uint32x4_t bp = vreinterpretq_u32_u16(vcgeq_u16(vabdq_u16(a, p), vt));
    const uint32x4_t br = vreinterpretq_u32_u16(vcgeq_u16(vabdq_u16(a, r), vt));
    chP = vsubq_u32(chP, vtstq_u32(bp, bp));  // +1 where any lane reached its threshold
    chR = vsubq_u32(chR, vtstq_u32(br, br));
  }
  vsPrev = int(vaddvq_u32(chP));
  vsRef = int(vaddvq_u32(chR));
#else
  int np = 0, nr = 0;
  for (int k = 0; k < kBlockPx; ++k) {
    const int l = int(im[k] & 0xFFFF), m = int(im[k] >> 16);
    const int pl = int(pm[k] & 0xFFFF), pr = int(pm[k] >> 16), rl = int(rm[k] & 0xFFFF), rr = int(rm[k] >> 16);
    np += int((std::abs(l - pl) >= tl) | (std::abs(m - pr) >= tr));
    nr += int((std::abs(l - rl) >= tl) | (std::abs(m - rr) >= tr));
  }
  vsPrev = np;
  vsRef = nr;
#endif
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

  for (int by = 0; by < kRows; ++by) {
    // The input is read block by block; fetch the next 16 scanlines (contiguous) ahead of time.
    if (by + 1 < kRows) prefetchRange(in + size_t(by + 1) * kBlock * kWidth, size_t(kBlock) * kWidth * sizeof(uint32_t));
    for (int bx = 0; bx < kCols; ++bx) measureBlock(in, bx, by);
  }
  const std::array<Stats, kBlocks>& inS = inS_;

  if (!primed_) {
    // First frame after a reset: shown as is; it is the reference for the trackers.
    if (in != out) std::memcpy(out, in, npx * sizeof(uint32_t));
    std::memcpy(prev_.data(), out, npx * sizeof(uint32_t));
    prevM_ = inM_;
    refM_ = inM_;
    prevIsLastIn_.fill(1);
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
        int vsPrev = 0, vsRef = 0;
        changedPixels(i, vsPrev, vsRef);
        px = changed[i] = std::max(vsPrev, vsRef);
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
          prepareBlend(in, bx, by);
          int lo = 0, hi = 256;  // invariant: ok(lo) (lo = previous output), !ok(hi)
          while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            if (ok(blendStats(mid))) lo = mid;
            else hi = mid;
          }
          a = lo;
          shown = writeBlend(out, bx, by, a);  // also updates prevM_ of the block
        }
        if (mode == Hold) b.stickyUntil = f + p_.stickyFrames;
      }

      // The displayed block becomes prev_ (pixels and per-pixel metrics).
      const size_t o = i * kBlockPx;
      for (int y = 0; y < kBlock; ++y) {
        const size_t off = size_t(by * kBlock + y) * kWidth + size_t(bx) * kBlock;
        if (a == 256 && in != out) std::memcpy(out + off, in + off, kBlock * sizeof(uint32_t));
        std::memcpy(prev_.data() + off, out + off, kBlock * sizeof(uint32_t));
      }
      if (a == 256) std::memcpy(prevM_.data() + o, inM_.data() + o, kBlockPx * sizeof(uint32_t));
      else ++info.alteredBlocks;
      prevIsLastIn_[i] = a == 256;

      const Tracker before = b.lum;
      bool tl = update(b.lum, shown.lum, shown.sat, false);
      // The reference (the displayed block when the luminance extremum was set) is only used
      // through its per-pixel metrics.
      if (b.lum.ext != before.ext || b.lum.lo != before.lo || b.lum.hi != before.hi || b.lum.dir != before.dir)
        std::memcpy(refM_.data() + o, prevM_.data() + o, kBlockPx * sizeof(uint32_t));
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
  info.altered = info.alteredBlocks > 0;
  return info;
}

}  // namespace rn
