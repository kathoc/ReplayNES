// Bit-exactness of the (optimised) flash reduction filter: engine/src/video/FlashFilter must produce
// exactly the same output pixels and FlashFrameInfo, frame by frame, as the frozen reference copy of
// the original implementation (tests/support/FlashFilterRef.*), over long synthetic flashing
// sequences (all levels, strobes, partial areas, red flashes, noise, fades, resets, level changes,
// in-place processing) and over the generated test ROM's frames (engine C API).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "replaynes/replaynes.h"
#include "support/FlashFilterRef.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"
#include "video/FlashFilter.h"

using namespace rntest;

namespace {
constexpr int W = 256, H = 240, NPX = W * H;
using Frame = std::vector<uint32_t>;

uint32_t rgb(int r, int g, int b) { return 0xFF000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b); }

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return uint32_t(s >> 11);
  }
  int range(int n) { return int(next() % uint32_t(n)); }
  uint32_t color() { return rgb(range(256), range(256), range(256)); }
};

// A small NES-like palette plus extremes and saturated reds.
const uint32_t kPalette[] = {rgb(0, 0, 0),       rgb(255, 255, 255), rgb(84, 84, 84),   rgb(152, 150, 152),
                             rgb(236, 238, 236), rgb(160, 26, 32),   rgb(255, 0, 0),    rgb(200, 10, 10),
                             rgb(48, 50, 236),   rgb(76, 154, 236),  rgb(56, 108, 0),   rgb(128, 208, 16),
                             rgb(236, 88, 180),  rgb(252, 252, 150), rgb(30, 0, 0),     rgb(120, 60, 0)};
uint32_t pal(Rng& r) { return kPalette[r.range(int(sizeof(kPalette) / sizeof(kPalette[0])))]; }

void fillRect(Frame& f, int x0, int y0, int w, int h, uint32_t c) {
  for (int y = std::max(0, y0); y < std::min(H, y0 + h); ++y)
    for (int x = std::max(0, x0); x < std::min(W, x0 + w); ++x) f[size_t(y * W + x)] = c;
}

void noiseRect(Frame& f, Rng& r, int x0, int y0, int w, int h, bool palette) {
  for (int y = std::max(0, y0); y < std::min(H, y0 + h); ++y)
    for (int x = std::max(0, x0); x < std::min(W, x0 + w); ++x) f[size_t(y * W + x)] = palette ? pal(r) : r.color();
}

// Mostly static tiled background with a few colours (like NES content).
Frame background(Rng& r) {
  Frame f(NPX);
  uint32_t c[4] = {pal(r), pal(r), pal(r), pal(r)};
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) f[size_t(y * W + x)] = c[((x / 8) ^ (y / 8) ^ (x * y / 37)) & 3];
  return f;
}

using Generator = Frame (*)(Rng&, int frame, Frame& state);

// Full-screen strobe between two colours, random period (1..6 frames) that changes now and then.
Frame genStrobe(Rng& r, int f, Frame& st) {
  if (st.size() < 4 || r.range(90) == 0) st = {r.color(), r.color(), uint32_t(1 + r.range(6)), 0};
  return Frame(NPX, st[(f / int(st[2])) % 2]);
}
// Partial-area flash (random rectangle, sometimes > large-area share) on a static background.
Frame genPartial(Rng& r, int f, Frame& st) {
  static Frame bg;
  if (f == 0 || bg.empty()) bg = background(r);
  if (st.size() < 8 || r.range(60) == 0)
    st = {uint32_t(r.range(W)), uint32_t(r.range(H)), uint32_t(16 + r.range(W)), uint32_t(16 + r.range(H)),
          pal(r), pal(r), uint32_t(1 + r.range(4)), 0};
  Frame o = bg;
  fillRect(o, int(st[0]) - 64, int(st[1]) - 60, int(st[2]), int(st[3]), st[4 + (f / int(st[6])) % 2]);
  return o;
}
// Saturated red flashes (red vs dark / red vs grey / red vs other red), full or partial.
Frame genRed(Rng& r, int f, Frame& st) {
  if (st.size() < 4 || r.range(70) == 0) {
    uint32_t reds[] = {rgb(255, 0, 0), rgb(200, 0, 0), rgb(160, 26, 32), rgb(255, 40, 30)};
    uint32_t others[] = {rgb(0, 0, 0), rgb(40, 0, 0), rgb(120, 120, 120), rgb(255, 0, 0), rgb(255, 255, 255)};
    st = {reds[r.range(4)], others[r.range(5)], uint32_t(1 + r.range(5)), uint32_t(r.range(2))};
  }
  Frame o(NPX, rgb(10, 10, 10));
  uint32_t c = st[(f / int(st[2])) % 2];
  if (st[3]) fillRect(o, 0, 0, W, H, c);
  else fillRect(o, 32, 30, 160, 150, c);
  return o;
}
// Random per-pixel noise over a large area alternating with a solid colour (exercises every
// channel value in the linear-light blend and the per-pixel change counting).
Frame genNoise(Rng& r, int f, Frame& st) {
  if (st.size() < 2 || r.range(50) == 0) st = {uint32_t(1 + r.range(3)), r.color()};
  Frame o(NPX, st[1]);
  if ((f / int(st[0])) % 2 == 0) noiseRect(o, r, r.range(64), r.range(60), 128 + r.range(128), 120 + r.range(120), r.range(2));
  return o;
}
// Gradual fades and ramps between random colours (slow transitions, extremum tracking, sticky).
Frame genFade(Rng& r, int f, Frame& st) {
  if (st.size() < 4 || r.range(40) == 0) st = {r.color(), r.color(), uint32_t(2 + r.range(12)), 0};
  int len = int(st[2]), k = f % (2 * len);
  int t = k < len ? k : 2 * len - k;
  auto mix = [&](int sh) {
    int a = int(st[0] >> sh & 255), b = int(st[1] >> sh & 255);
    return a + (b - a) * t / len;
  };
  Frame o(NPX, rgb(mix(16), mix(8), mix(0)));
  if (r.range(3) == 0) noiseRect(o, r, 0, 0, W, 40, true);
  return o;
}
// Chaotic: every frame a random combination of blocks/rects of palette colours and occasional
// repeats of the previous frame (unchanged blocks).
Frame genChaos(Rng& r, int f, Frame& st) {
  if (st.size() != size_t(NPX)) st = Frame(NPX, 0xFF000000u);
  int mode = r.range(6);
  if (mode == 0) return st;  // identical to the previous frame
  Frame o = st;
  int n = 1 + r.range(12);
  for (int i = 0; i < n; ++i) fillRect(o, r.range(W) - 32, r.range(H) - 32, 8 + r.range(200), 8 + r.range(200), mode == 1 ? r.color() : pal(r));
  if (mode == 2) noiseRect(o, r, r.range(W), r.range(H), 40, 40, false);
  st = o;
  return o;
}

struct Diff {
  int frames = 0, mismatches = 0, firstBad = -1, alteredFrames = 0, largeFrames = 0;
};

bool sameInfo(const rn::FlashFrameInfo& a, const rnref::FlashFrameInfo& b) {
  return a.altered == b.altered && a.alteredBlocks == b.alteredBlocks && a.eventAreaPermille == b.eventAreaPermille &&
         a.largeArea == b.largeArea;
}

// Feeds `in` to both filters (optionally in place) and compares.
void compareFrame(rn::FlashFilter& opt, rnref::FlashFilter& ref, const uint32_t* in, bool inPlace, int index, Diff& d) {
  Frame a(in, in + NPX), b(in, in + NPX), oa(NPX, 0xDEADBEEFu), ob(NPX, 0x12345678u);
  rn::FlashFrameInfo ia = inPlace ? opt.process(a.data(), a.data()) : opt.process(a.data(), oa.data());
  rnref::FlashFrameInfo ib = inPlace ? ref.process(b.data(), b.data()) : ref.process(b.data(), ob.data());
  const Frame& ra = inPlace ? a : oa;
  const Frame& rb = inPlace ? b : ob;
  ++d.frames;
  d.alteredFrames += ib.altered;
  d.largeFrames += ib.largeArea;
  if (ra != rb || !sameInfo(ia, ib)) {
    if (d.firstBad < 0) d.firstBad = index;
    ++d.mismatches;
  }
}

void runSynthetic(const char* name, Generator gen, uint64_t seed, int frames) {
  for (int lvl = 0; lvl <= 3; ++lvl) {
    Rng r(seed * 31 + uint64_t(lvl));
    rn::FlashFilter opt{rn::FlashLevel(lvl)};
    rnref::FlashFilter ref{rnref::FlashLevel(lvl)};
    Frame st;
    Diff d;
    for (int f = 0; f < frames; ++f) {
      Frame in = gen(r, f, st);
      if (r.range(400) == 0) {  // discontinuity
        opt.reset();
        ref.reset();
      }
      if (r.range(900) == 0) {  // level change (also resets), then back
        int l2 = r.range(4);
        opt.setLevel(rn::FlashLevel(l2));
        ref.setLevel(rnref::FlashLevel(l2));
      }
      compareFrame(opt, ref, in.data(), r.range(4) == 0, f, d);
      if (d.mismatches) break;
    }
    MESSAGE(std::string(name) + " level " + std::to_string(lvl) + ": " + std::to_string(d.frames) + " frames, " +
            std::to_string(d.alteredFrames) + " altered, " + std::to_string(d.largeFrames) + " large-area");
    if (d.mismatches) FAIL(std::string(name) + " level " + std::to_string(lvl) + ": first mismatch at frame " + std::to_string(d.firstBad));
  }
}
}  // namespace

TEST_CASE("flash filter exact: full-screen strobes") { runSynthetic("strobe", genStrobe, 1, 1500); }
TEST_CASE("flash filter exact: partial-area flashes") { runSynthetic("partial", genPartial, 2, 1500); }
TEST_CASE("flash filter exact: red flashes") { runSynthetic("red", genRed, 3, 1500); }
TEST_CASE("flash filter exact: noise flashes") { runSynthetic("noise", genNoise, 4, 900); }
TEST_CASE("flash filter exact: fades") { runSynthetic("fade", genFade, 5, 1200); }
TEST_CASE("flash filter exact: chaos with repeated frames") { runSynthetic("chaos", genChaos, 6, 1500); }

TEST_CASE("flash filter exact: static luts and per-pixel metrics") {
  for (int c = 0; c < 256; ++c) CHECK_EQ(rn::FlashFilter::toLinear(uint8_t(c)), rnref::FlashFilter::toLinear(uint8_t(c)));
  for (int v = 0; v < 65536; ++v) REQUIRE_EQ(rn::FlashFilter::toSrgb(uint16_t(v)), rnref::FlashFilter::toSrgb(uint16_t(v)));
  Rng r(77);
  for (int i = 0; i < 200000; ++i) {
    uint32_t px = r.color();
    REQUIRE_EQ(rn::FlashFilter::luminance(px), rnref::FlashFilter::luminance(px));
    REQUIRE_EQ(rn::FlashFilter::redMetric(px), rnref::FlashFilter::redMetric(px));
  }
  for (int lvl = 0; lvl <= 3; ++lvl) {
    rn::FlashParams a = rn::FlashFilter::params(rn::FlashLevel(lvl));
    rnref::FlashParams b = rnref::FlashFilter::params(rnref::FlashLevel(lvl));
    CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
  }
}

TEST_CASE("flash filter exact: generated test ROM frames") {
  std::string dir = tempDir("flash-exact");
  std::string rom = writeTestRom(dir);
  rn_session_options opt;
  rn_session_options_init(&opt);
  rn_session* s = nullptr;
  REQUIRE(rn_session_new(rom.c_str(), nullptr, &opt, &s) == RN_OK);
  constexpr int kFrames = 1500;
  std::vector<Frame> frames;
  frames.reserve(kFrames);
  for (int f = 0; f < kFrames; ++f) {
    // No input for the first 600 frames (the benchmark scenario), then varied input + a soft reset.
    uint8_t p1 = f < 600 ? 0 : uint8_t((f * 7) & 0xFF), ev = f == 1000 ? RN_EV_SOFT_RESET : 0;
    REQUIRE(rn_step(s, p1, 0, ev, nullptr) == RN_OK);
    const uint32_t* v = rn_video(s);
    frames.emplace_back(v, v + NPX);
  }
  rn_session_close(s);
  for (int lvl = 0; lvl <= 3; ++lvl) {
    rn::FlashFilter o{rn::FlashLevel(lvl)};
    rnref::FlashFilter r{rnref::FlashLevel(lvl)};
    Diff d;
    for (int f = 0; f < kFrames; ++f) {
      if (f == 1200) {
        o.reset();
        r.reset();
      }
      compareFrame(o, r, frames[size_t(f)].data(), f % 5 == 0, f, d);
      if (d.mismatches) break;
    }
    MESSAGE("test ROM level " + std::to_string(lvl) + ": " + std::to_string(d.frames) + " frames, " +
            std::to_string(d.alteredFrames) + " altered, " + std::to_string(d.largeFrames) + " large-area");
    if (d.mismatches) FAIL("test ROM level " + std::to_string(lvl) + ": first mismatch at frame " + std::to_string(d.firstBad));
    if (lvl >= 2) CHECK(d.alteredFrames > 100);  // the heavy suppression path is really exercised
  }
}
