// FROZEN reference copy of engine/src/video/FlashFilter.{h,cpp} as of commit 499f42f (before the
// speed optimisation), in namespace rnref. Used only by tests/test_flash_filter_exact.cpp to prove
// that the optimised engine filter is bit-exact. Do not change its behaviour.
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
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace rnref {

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

  int classify(const Tracker& t, int v, bool sat, bool isRed) const;  // +1/-1 transition, 0 none
  bool update(Tracker& t, int v, bool sat, bool isRed) const;          // true if a transition
  int opposingDistance(const Tracker& t, int v) const;
  int transitionsInWindow(const Block& b) const;
  static Stats blockStats(const uint32_t* frame, int bx, int by);
  // Pixels of the block whose luminance / red metric changes by at least the thresholds.
  int changedPixels(const uint32_t* in, const uint32_t* ref, int bx, int by) const;
  void copyBlock(const uint32_t* src, uint32_t* dst, int bx, int by) const;
  // Writes the blend of prev_ and in (weight a/256 of in) for one block into out; returns stats.
  Stats blendBlock(const uint32_t* in, uint32_t* out, int bx, int by, int a) const;

  FlashLevel level_;
  FlashParams p_;
  bool primed_ = false;
  int64_t frame_ = 0;
  std::vector<uint32_t> prev_;  // last displayed frame
  std::vector<uint32_t> ref_;   // per block: displayed pixels when its luminance extremum was set
  std::array<Block, kBlocks> blocks_;
};

}  // namespace rnref
