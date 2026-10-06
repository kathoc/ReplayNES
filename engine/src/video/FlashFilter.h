// Photosensitive flash reduction (display-side only).
//
// A pure, deterministic function of the sequence of frames it is given plus its settings: it never
// touches emulation state, recorded input or verification hashes. Frontends run it on the frames
// they *display* (live view, exported video) and reset it on discontinuities (seek, load, ...).
//
// Algorithm (details and rationale: docs/FLASH_REDUCTION.md):
//   * The 256x240 frame is split into 16x15 blocks of 16x16 px. Per block we measure the mean WCAG
//     relative luminance L (linear light) and a "saturated red" metric M = mean(max(0, R-G-B))
//     (linear light; WCAG's (R-G-B)*320 scale = M*320).
//   * Per block, an extremum tracker on the *displayed* values counts WCAG transitions: a change
//     of >= threshold in the direction opposite to the previous one (a flash = a pair of opposing
//     transitions). Transition frames are kept for a sliding 61-frame (> 1 s) window.
//   * A frame whose new transitions (plus those of the last 3 frames, same direction) cover at least
//     the "large area" share of the screen (counted in changed pixels, not whole blocks) is a
//     large-area flash candidate. Smaller areas (sprite
//     blinking, scrolling details) are always passed through unchanged.
//   * In a large-area event a block may make the transition only if its window still has budget;
//     the last unit of budget is reserved for a transition towards *darker* so a suppressed screen
//     settles on the darker state. Otherwise the block is suppressed: it is blended in linear light
//     between the previously displayed frame and the new one with the largest weight that keeps
//     the change below the hold limit (no transition) and below a per-frame rate limit. Suppressed
//     blocks stay rate-limited ("sticky") for a few frames so the residual flicker stays tiny.
//   * Blocks that are not suppressed are copied bit-exactly, so normal content is untouched.
// All arithmetic is integer (16-bit linear light) for bit-identical results on every platform.
//
// Implementation: the per-pixel metrics of the input, the shown frame and the references are cached
// (block-major arrays), blocks equal to the last input are not measured again, pixel metrics are
// memoised per pair of adjacent pixels, and the hold/smooth bisection works on the distinct
// (previous, new) pixel values of a block instead of on every pixel. All of it computes exactly the
// same integers as the straightforward per-pixel formulation (tests/test_flash_filter_exact.cpp
// compares against a frozen copy of it).
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace rn {

enum class FlashLevel : int { Off = 0, Low = 1, Standard = 2, High = 3 };

// Linear-light quantities use 0..65535 for 0..1.
struct FlashParams {
  int lumThreshold = 0;          // luminance change that counts as a transition
  bool lumRequireDark = false;   // WCAG: the darker state must be below 0.80 (Low only)
  int redThreshold = 0;          // red metric change that counts as a transition (WCAG 20/320)
  bool redRequireSaturation = false;  // WCAG: R/(R+G+B) >= 0.8 in either state (Low only)
  int budget = 0;                // transitions per block allowed in any window (large-area events)
  int areaPermille = 0;          // large-area threshold, per mille of the screen
  int lumHold = 0, redHold = 0;  // max opposing move from the extremum while suppressed
  int lumRate = 0, redRate = 0;  // max change per frame while suppressed / sticky
  int stickyFrames = 0;          // frames a suppressed block stays rate-limited
};

struct FlashFrameInfo {
  bool altered = false;          // output differs from the input
  int alteredBlocks = 0;
  int eventAreaPermille = 0;     // largest same-direction transition area this frame
  bool largeArea = false;        // a large-area flash candidate was detected
};

class FlashFilter {
 public:
  static constexpr int kWidth = 256, kHeight = 240;
  static constexpr int kBlock = 16, kCols = kWidth / kBlock, kRows = kHeight / kBlock, kBlocks = kCols * kRows;
  static constexpr int kWindow = 61;   // frames; > 1 s at 60.0988 Hz
  static constexpr int kRecent = 3;    // frames over which simultaneous transitions add up
  static constexpr int kHistory = 16;  // transition frames remembered per block (>= any budget)

  explicit FlashFilter(FlashLevel level = FlashLevel::Standard);
  void setLevel(FlashLevel level);  // also resets
  FlashLevel level() const { return level_; }
  void reset();
  // in/out: 256x240 BGRA8 (0xAARRGGBB little-endian). in may equal out.
  FlashFrameInfo process(const uint32_t* in, uint32_t* out);

  static FlashParams params(FlashLevel level);
  static uint16_t toLinear(uint8_t srgb);
  static uint8_t toSrgb(uint16_t linear);
  // WCAG relative luminance (0..65535) of one BGRA pixel.
  static int luminance(uint32_t px);
  static int redMetric(uint32_t px);  // max(0, R-G-B) in linear light, 0..65535

 private:
  struct Tracker {
    int ext = 0, lo = 0, hi = 0;
    int dir = 0;  // +1 rising, -1 falling, 0 not yet moved
    bool extSat = false, loSat = false, hiSat = false;
  };
  struct Stats {
    int lum = 0, red = 0;
    bool sat = false;  // block-mean colour is saturated red (R/(R+G+B) >= 0.8)
  };
  struct Block {
    Tracker lum, red;
    Stats shown;  // stats of the last displayed block
    std::array<int64_t, kHistory> hist{};
    int histCount = 0, histHead = 0;
    int64_t lastTransition = -1000000;
    int lastDir = 0;
    int lastChanged = 0;  // changed pixels of the last transition (area accounting)
    int64_t stickyUntil = -1;
  };

  static constexpr int kBlockPx = kBlock * kBlock;

  int classify(const Tracker& t, int v, bool sat, bool isRed) const;  // +1/-1 transition, 0 none
  bool update(Tracker& t, int v, bool sat, bool isRed) const;          // true if a transition
  int opposingDistance(const Tracker& t, int v) const;
  int transitionsInWindow(const Block& b) const;
  // Stats of one block of `in` (inS_) plus its per-pixel luminance / red metric (inM_). A block
  // equal to the previous input reuses what was measured then.
  void measureBlock(const uint32_t* in, int bx, int by);
  // Metrics of two horizontally adjacent pixels (a pure function of their values), memoised in a
  // direct-mapped cache. The all-zero entry is exact for two pixels 0, so the cache starts valid.
  struct PairEntry {
    uint64_t key = 0;     // the two pixels as stored in memory
    uint32_t lr[2] = {};  // per pixel: luminance | red metric << 16
    uint64_t s0 = 0;      // sum of luminance | sum of red metric << 32
    uint64_t s1 = 0;      // sum of linear R | sum of linear R+G+B << 32 (block saturation test)
  };
  static constexpr int kPairCacheBits = 11;
  static PairEntry measurePair(const uint32_t* px, uint64_t key);
  // Pixels of block i whose luminance / red metric changes by at least the thresholds, against the
  // previous output (first) and the block's reference (second). Uses the per-pixel caches.
  void changedPixels(size_t i, int& vsPrev, int& vsRef) const;
  // Blend of prev_ and in (weight a/256 of in) for one block, in linear light, quantised to sRGB.
  // prepareBlend() reduces the block to its distinct (prev, in) pixel value pairs ("units");
  // blendStats(a) gives the stats of the quantised blend; writeBlend(a) writes it to out and the
  // block's per-pixel metrics to prevM_, and returns its stats.
  void prepareBlend(const uint32_t* in, int bx, int by);
  Stats blendStats(int a) const;
  Stats writeBlend(uint32_t* out, int bx, int by, int a);

  FlashLevel level_;
  FlashParams p_;
  bool primed_ = false;
  int64_t frame_ = 0;
  std::vector<uint32_t> prev_;  // last displayed frame
  std::array<Block, kBlocks> blocks_;

  // Per-pixel luminance | red metric << 16, block-major (block i = [i*kBlockPx, (i+1)*kBlockPx)),
  // of the current input, the last displayed frame (prev_) and, per block, the reference: the
  // displayed block when its luminance extremum was set.
  std::vector<uint32_t> inM_, prevM_, refM_;
  // Speed-only caches (pure functions of pixel content; they never change the output).
  std::array<Stats, kBlocks> inS_{};       // stats of the input block that inM_ describes
  std::array<char, kBlocks> prevIsLastIn_{};  // prev_ block == input block inS_/inM_ describe
  int directBlocks_ = 0;                   // blocks left to measure without the pair cache
  int identityBlocks_ = 0;                 // blocks left to blend without deduplication
  std::vector<PairEntry> pairCache_;
  // Blend scratch of one block (see prepareBlend).
  struct BlendScratch {
    int units = 0, pairs = 0;
    // Per unit (distinct (prev, in) pixel value pair) and channel: lin(prev) * 256 + 128 and
    // lin(in) - lin(prev), so that the blend (lin(prev) * (256 - a) + lin(in) * a + 128) >> 8 is
    // (base + diff * a) >> 8; and its pixel count.
    int32_t base[3][kBlockPx], diff[3][kBlockPx], count[kBlockPx];
    uint16_t pairUnit[kBlockPx / 2][2];  // per pixel-pair unit: the units of its two pixels
    uint16_t pairOf[kBlockPx / 2];       // per pixel pair of the block (row-major): its pair unit
    // Hash tables (tag = generation << 9 | index): units (512 slots), pair units (256 slots).
    uint64_t key[2 * kBlockPx];
    uint32_t tag[2 * kBlockPx] = {};
    uint64_t pkey[kBlockPx][2];
    uint32_t ptag[kBlockPx] = {};
    uint32_t gen = 0;
  };
  std::vector<BlendScratch> blend_;  // one element (heap: keeps the object small and movable)
};

}  // namespace rn
