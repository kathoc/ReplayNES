// Photosensitive flash reduction filter (engine/src/video/FlashFilter, C API rn_flash_filter_*).
// Synthetic frames are measured with an independent WCAG 2.x flash counter (double precision,
// 8x8 px cells, its own sRGB conversion) - not with the filter's own bookkeeping.
// With <repo>/roms present (developer's own ROMs, gitignored) it also runs real games through the
// filter; RN_FLASH_DUMP_DIR=<dir> writes PPM pictures of altered frames for manual inspection.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "replaynes/replaynes.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"
#include "video/FlashFilter.h"

using namespace rn;
using namespace rntest;

namespace {
constexpr int W = 256, H = 240, NPX = W * H;
using Frame = std::vector<uint32_t>;

uint32_t rgb(int r, int g, int b) { return 0xFF000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b); }
Frame solid(uint32_t c) { return Frame(NPX, c); }

// ---------------------------------------------------------------- independent WCAG counter
double srgbToLinear(int c) {
  double s = c / 255.0;
  return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}

struct WcagResult {
  int generalFlashes = 0;  // max over 60-frame windows of flashes on >= 25% of the screen
  int redFlashes = 0;
};

// Per 8x8 cell: relative luminance and (R-G-B)*320 (negative -> 0), WCAG 2.2 definitions.
// Transitions via extremum tracking: luminance change >= 0.1 with the darker state < 0.8;
// red change > 20 with R/(R+G+B) >= 0.8 in either state. Flashes = ceil(transitions / 2).
WcagResult wcagCount(const std::vector<Frame>& frames) {
  constexpr int C = 8, CW = W / C, CH = H / C, NC = CW * CH;
  const size_t n = frames.size();
  std::vector<std::vector<double>> lum(NC, std::vector<double>(n)), red(NC, std::vector<double>(n));
  std::vector<std::vector<char>> sat(NC, std::vector<char>(n));
  for (size_t f = 0; f < n; ++f) {
    for (int cy = 0; cy < CH; ++cy)
      for (int cx = 0; cx < CW; ++cx) {
        double sl = 0, sr = 0, sg = 0, sb = 0, sm = 0;
        for (int y = 0; y < C; ++y)
          for (int x = 0; x < C; ++x) {
            uint32_t p = frames[f][size_t((cy * C + y) * W + cx * C + x)];
            double r = srgbToLinear(int(p >> 16 & 255)), g = srgbToLinear(int(p >> 8 & 255)), b = srgbToLinear(int(p & 255));
            sl += 0.2126 * r + 0.7152 * g + 0.0722 * b;
            sm += std::max(0.0, (r - g - b) * 320.0);
            sr += r, sg += g, sb += b;
          }
        int c = cy * CW + cx;
        lum[size_t(c)][f] = sl / (C * C);
        red[size_t(c)][f] = sm / (C * C);
        sat[size_t(c)][f] = (sr + sg + sb) > 0 && sr / (sr + sg + sb) >= 0.8;
      }
  }
  auto transitions = [&](const std::vector<double>& v, const std::vector<char>& s, bool isRed) {
    std::vector<size_t> out;
    double ext = v[0], lo = v[0], hi = v[0];
    bool extSat = s[0], loSat = s[0], hiSat = s[0];
    int dir = 0;
    auto ok = [&](double darker, bool satA, bool satB) { return isRed ? (satA || satB) : darker < 0.8; };
    auto big = [&](double d) { return isRed ? d > 20.0 : d >= 0.1; };
    for (size_t f = 1; f < v.size(); ++f) {
      double x = v[f];
      int t = 0;
      if (dir == 0) {
        if (big(x - lo) && ok(lo, loSat, s[f])) t = +1;
        else if (big(hi - x) && ok(x, hiSat, s[f])) t = -1;
        else {
          if (x < lo) lo = x, loSat = s[f];
          if (x > hi) hi = x, hiSat = s[f];
        }
      } else if (dir > 0) {
        if (x > ext) ext = x, extSat = s[f];
        else if (big(ext - x) && ok(x, extSat, s[f])) t = -1;
      } else {
        if (x < ext) ext = x, extSat = s[f];
        else if (big(x - ext) && ok(ext, extSat, s[f])) t = +1;
      }
      if (t != 0) {
        dir = t, ext = x, extSat = s[f];
        out.push_back(f);
      }
    }
    return out;
  };
  WcagResult res;
  for (int kind = 0; kind < 2; ++kind) {
    std::vector<std::vector<size_t>> tr(NC);
    for (int c = 0; c < NC; ++c) tr[size_t(c)] = transitions(kind ? red[size_t(c)] : lum[size_t(c)], sat[size_t(c)], kind == 1);
    int worst = 0;
    for (size_t w = 0; w + 1 < std::max<size_t>(n, 61) - 59; ++w) {
      std::vector<int> counts(NC);
      for (int c = 0; c < NC; ++c)
        for (size_t f : tr[size_t(c)])
          if (f >= w && f < w + 60) ++counts[size_t(c)];
      std::sort(counts.begin(), counts.end(), std::greater<int>());
      int quarter = counts[size_t(NC / 4 - 1)];  // transitions shared by >= 25% of the screen
      worst = std::max(worst, (quarter + 1) / 2);
    }
    (kind ? res.redFlashes : res.generalFlashes) = worst;
  }
  return res;
}

struct Run {
  std::vector<Frame> out;
  int alteredFrames = 0;
};
Run runFilter(FlashLevel level, const std::vector<Frame>& in) {
  FlashFilter filter(level);
  Run r;
  for (auto& f : in) {
    Frame o(NPX);
    if (filter.process(f.data(), o.data()).altered) ++r.alteredFrames;
    r.out.push_back(std::move(o));
  }
  return r;
}

std::vector<Frame> alternating(uint32_t a, uint32_t b, int frames, int period = 1) {
  std::vector<Frame> v;
  for (int i = 0; i < frames; ++i) v.push_back(solid((i / period) % 2 ? b : a));
  return v;
}

// A Super-Mario-like scrolling level: sky, clouds, hills, pipes, brick ground (wraps every 512 px).
uint32_t scenePixel(int wx, int y) {
  wx = ((wx % 512) + 512) % 512;
  if (y >= 200) {  // bricks: 16x8 with mortar lines
    bool mortar = (y % 8) == 0 || ((wx + ((y / 8) % 2) * 8) % 16) == 0;
    return mortar ? rgb(0, 0, 0) : rgb(200, 76, 12);
  }
  int px = wx % 256;
  if (px >= 180 && px < 212 && y >= 140) {  // pipe with dark outline
    if (px == 180 || px == 211 || y == 140) return rgb(0, 0, 0);
    return px < 192 ? rgb(184, 248, 24) : rgb(0, 168, 0);
  }
  int hx = (wx % 384) - 80, hy = y - 200;  // hill
  if (hx * hx / 4 + hy * hy < 1600) return rgb(0, 168, 0);
  int cx = (wx % 320) - 100, cy = y - 50;  // cloud
  if (cx * cx / 9 + cy * cy * 2 < 300) return cy > 4 ? rgb(60, 188, 252) : rgb(252, 252, 252);
  return rgb(92, 148, 252);
}
Frame scene(int scroll, int scrollY = 0) {
  Frame f(NPX);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) f[size_t(y * W + x)] = scenePixel(x + scroll, std::min(H - 1, std::max(0, y + scrollY)));
  return f;
}

void writePPM(const std::string& path, const Frame& a, const Frame& b) {  // side by side
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return;
  std::fprintf(fp, "P6\n%d %d\n255\n", 2 * W + 8, H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < 2 * W + 8; ++x) {
      uint32_t p = x < W ? a[size_t(y * W + x)] : x >= W + 8 ? b[size_t(y * W + x - W - 8)] : rgb(255, 0, 255);
      unsigned char c[3] = {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p)};
      std::fwrite(c, 1, 3, fp);
    }
  std::fclose(fp);
}
}  // namespace

TEST_CASE("flash filter: sRGB tables round-trip exactly") {
  for (int c = 0; c < 256; ++c) CHECK_EQ(int(FlashFilter::toSrgb(FlashFilter::toLinear(uint8_t(c)))), c);
  CHECK_EQ(FlashFilter::luminance(rgb(255, 255, 255)), 65535);
  CHECK_EQ(FlashFilter::luminance(rgb(0, 0, 0)), 0);
}

TEST_CASE("flash filter: counter sanity on unfiltered input") {
  auto bw = wcagCount(alternating(rgb(0, 0, 0), rgb(255, 255, 255), 120));
  CHECK(bw.generalFlashes >= 25);
  auto red = wcagCount(alternating(rgb(127, 127, 127), rgb(255, 0, 0), 120));
  CHECK(red.redFlashes >= 25);
  CHECK(red.generalFlashes == 0);  // luminance of grey 127 ~= pure red: a red-only flash
}

TEST_CASE("flash filter: full-screen black/white at 30 Hz is limited to <= 3 flashes/s") {
  auto in = alternating(rgb(0, 0, 0), rgb(255, 255, 255), 180);
  for (FlashLevel lv : {FlashLevel::Low, FlashLevel::Standard, FlashLevel::High}) {
    Run r = runFilter(lv, in);
    WcagResult w = wcagCount(r.out);
    MESSAGE("level " << int(lv) << ": flashes " << w.generalFlashes << ", altered frames " << r.alteredFrames);
    CHECK(w.generalFlashes <= 3);
    if (lv == FlashLevel::Standard) CHECK(w.generalFlashes <= 2);
    if (lv == FlashLevel::High) CHECK(w.generalFlashes <= 1);
    CHECK(r.alteredFrames > 60);
  }
  // Slower flashing (every 4 frames = 7.5 Hz) and a mid-grey pair are limited too.
  for (auto seq : {alternating(rgb(0, 0, 0), rgb(255, 255, 255), 240, 4), alternating(rgb(40, 40, 40), rgb(160, 160, 160), 180)}) {
    for (FlashLevel lv : {FlashLevel::Low, FlashLevel::Standard, FlashLevel::High}) {
      CHECK(wcagCount(seq).generalFlashes > 3);
      CHECK(wcagCount(runFilter(lv, seq).out).generalFlashes <= 3);
    }
  }
  // Off: untouched.
  Run off = runFilter(FlashLevel::Off, in);
  CHECK(off.out == in);
}

TEST_CASE("flash filter: saturated red flashes are limited") {
  for (auto seq : {alternating(rgb(127, 127, 127), rgb(255, 0, 0), 180), alternating(rgb(0, 0, 0), rgb(255, 0, 0), 180),
                   alternating(rgb(60, 0, 0), rgb(255, 20, 20), 180, 3)}) {
    for (FlashLevel lv : {FlashLevel::Low, FlashLevel::Standard, FlashLevel::High}) {
      WcagResult w = wcagCount(runFilter(lv, seq).out);
      MESSAGE("red level " << int(lv) << ": red " << w.redFlashes << " general " << w.generalFlashes);
      CHECK(w.redFlashes <= 3);
      CHECK(w.generalFlashes <= 3);
    }
  }
}

TEST_CASE("flash filter: half-screen flash and a gradual (sinusoidal) flash are limited") {
  std::vector<Frame> half, sine;
  Frame base = scene(0);
  for (int i = 0; i < 180; ++i) {
    Frame f = base;
    if (i % 2)
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W / 2; ++x) f[size_t(y * W + x)] = rgb(255, 255, 255);
    half.push_back(f);
    int v = int(std::lround(128 + 100 * std::sin(i * 2 * 3.14159265358979 * 4.5 / 60.0)));  // 4.5 Hz
    sine.push_back(solid(rgb(v, v, v)));
  }
  CHECK(wcagCount(half).generalFlashes > 3);
  CHECK(wcagCount(sine).generalFlashes > 3);
  for (FlashLevel lv : {FlashLevel::Low, FlashLevel::Standard, FlashLevel::High}) {
    CHECK(wcagCount(runFilter(lv, half).out).generalFlashes <= 3);
    CHECK(wcagCount(runFilter(lv, sine).out).generalFlashes <= 3);
  }
}

TEST_CASE("flash filter: static, scrolling and fading content is bit-exact") {
  std::vector<std::vector<Frame>> cases;
  cases.push_back(std::vector<Frame>(200, scene(37)));
  for (int speed : {1, 2, 3, 5}) {
    std::vector<Frame> v;
    for (int i = 0; i < 400; ++i) v.push_back(scene(i * speed));
    cases.push_back(v);
  }
  {
    std::vector<Frame> v;  // vertical scroll
    for (int i = 0; i < 160; ++i) v.push_back(scene(0, std::abs((i % 80) - 40) - 20));  // triangle, 1 px/frame
    cases.push_back(v);
  }
  {
    std::vector<Frame> v;  // NES-style 4-step fade in, play, fade out (one transition each way)
    Frame s = scene(0);
    auto dim = [&](int k) {
      Frame f = s;
      for (auto& p : f) p = rgb(int(p >> 16 & 255) * k / 4, int(p >> 8 & 255) * k / 4, int(p & 255) * k / 4);
      return f;
    };
    for (int k = 0; k <= 4; ++k)
      for (int i = 0; i < 4; ++i) v.push_back(dim(k));
    for (int i = 0; i < 60; ++i) v.push_back(scene(i));
    for (int k = 4; k >= 0; --k)
      for (int i = 0; i < 4; ++i) v.push_back(dim(k));
    cases.push_back(v);
  }
  {
    std::vector<Frame> v;  // a single scene cut and back after a second
    for (int i = 0; i < 120; ++i) v.push_back(i >= 30 && i < 90 ? solid(rgb(0, 0, 0)) : scene(i));
    cases.push_back(v);
  }
  for (FlashLevel lv : {FlashLevel::Low, FlashLevel::Standard, FlashLevel::High})
    for (size_t c = 0; c < cases.size(); ++c) {
      Run r = runFilter(lv, cases[c]);
      if (r.out != cases[c]) MESSAGE("level " << int(lv) << " case " << c << " altered frames " << r.alteredFrames);
      CHECK(r.out == cases[c]);
    }
}

TEST_CASE("flash filter: small-area flicker (sprite blink, < 20% of the screen) is not altered") {
  auto blink = [&](int x0, int y0, int w, int h, uint32_t on, uint32_t off, int period) {
    std::vector<Frame> v;
    for (int i = 0; i < 180; ++i) {
      Frame f = scene(i);
      uint32_t c = (i / period) % 2 ? on : off;
      for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) f[size_t(y * W + x)] = c;
      v.push_back(f);
    }
    return v;
  };
  // 16x16 sprite, any level.
  auto sprite = blink(100, 120, 16, 16, rgb(255, 255, 255), rgb(0, 0, 0), 1);
  for (FlashLevel lv : {FlashLevel::Low, FlashLevel::Standard, FlashLevel::High}) CHECK(runFilter(lv, sprite).out == sprite);
  // 96x96 (15% of the screen), block-aligned and not, black/white and red: Low and Standard.
  for (auto seq : {blink(64, 64, 96, 96, rgb(255, 255, 255), rgb(0, 0, 0), 1), blink(72, 56, 96, 96, rgb(255, 255, 255), rgb(0, 0, 0), 2),
                   blink(40, 40, 96, 96, rgb(255, 0, 0), rgb(0, 0, 0), 1)}) {
    CHECK(wcagCount(seq).generalFlashes <= 3);  // below the 25% area: not a WCAG general flash
    for (FlashLevel lv : {FlashLevel::Low, FlashLevel::Standard}) CHECK(runFilter(lv, seq).out == seq);
  }
}

TEST_CASE("flash filter: deterministic, reset restarts, in-place processing") {
  std::vector<Frame> seq;
  for (int i = 0; i < 150; ++i) seq.push_back(i % 3 == 0 ? solid(rgb(255, 255, 255)) : scene(i * 2));
  Run a = runFilter(FlashLevel::Standard, seq), b = runFilter(FlashLevel::Standard, seq);
  CHECK(a.out == b.out);
  CHECK(a.alteredFrames > 0);

  FlashFilter f(FlashLevel::Standard);
  for (int i = 0; i < 70; ++i) {
    Frame o(NPX);
    f.process(seq[size_t(i)].data(), o.data());
  }
  f.reset();
  for (size_t i = 0; i < seq.size(); ++i) {
    Frame o = seq[i];
    f.process(o.data(), o.data());  // in == out
    CHECK(o == a.out[i]);
  }
}

TEST_CASE("flash filter: C API and session hashes are unaffected") {
  std::string dir = tempDir("flash");
  std::string rom = writeTestRom(dir);
  rn_session_options opt;
  rn_session_options_init(&opt);
  rn_session *s1 = nullptr, *s2 = nullptr;
  REQUIRE(rn_session_new(rom.c_str(), nullptr, &opt, &s1) == RN_OK);
  REQUIRE(rn_session_new(rom.c_str(), nullptr, &opt, &s2) == RN_OK);
  rn_flash_filter* ff = rn_flash_filter_new(RN_FLASH_STANDARD);
  REQUIRE(ff != nullptr);
  CHECK(rn_flash_filter_new(rn_flash_level(7)) == nullptr);
  CHECK(rn_flash_filter_set_level(ff, rn_flash_level(9)) == RN_ERR_INVALID_ARG);
  CHECK(rn_flash_filter_get_level(ff) == RN_FLASH_STANDARD);
  CHECK(rn_flash_filter_process(ff, nullptr, nullptr, nullptr) == RN_ERR_INVALID_ARG);
  Frame out(NPX);
  for (int f = 0; f < 600; ++f) {
    uint8_t p1 = uint8_t((f * 7) & 0xFF), ev = f == 300 ? RN_EV_SOFT_RESET : 0;
    REQUIRE(rn_step(s1, p1, 0, ev, nullptr) == RN_OK);
    REQUIRE(rn_step(s2, p1, 0, ev, nullptr) == RN_OK);
    uint64_t vh = rn_video_hash(s2);
    rn_flash_info info;
    REQUIRE(rn_flash_filter_process(ff, rn_video(s2), out.data(), &info) == RN_OK);
    if (f % 97 == 0) {
      rn_flash_filter_reset(ff);
      REQUIRE(rn_flash_filter_set_level(ff, f % 2 ? RN_FLASH_HIGH : RN_FLASH_STANDARD) == RN_OK);
    }
    CHECK_EQ(rn_video_hash(s2), vh);
    CHECK_EQ(rn_video_hash(s1), rn_video_hash(s2));
    if (f % 50 == 0) CHECK_EQ(rn_state_hash(s1), rn_state_hash(s2));
  }
  CHECK_EQ(rn_state_hash(s1), rn_state_hash(s2));
  CHECK_EQ(rn_audio_hash(s1), rn_audio_hash(s2));
  rn_flash_filter_free(ff);
  rn_session_close(s1);
  rn_session_close(s2);
}

// ---------------------------------------------------------------- developer's own ROMs (optional)
namespace {
struct RomCase {
  const char* name;
  int frames;
  std::function<uint8_t(int)> input;
};
}  // namespace

TEST_CASE("flash filter: local ROMs (skipped without <repo>/roms)") {
  std::vector<std::string> names;
  if (!fs::listDir(RN_LOCAL_ROMS, names).ok() || names.empty()) {
    MESSAGE(std::string("no ROMs in ") + RN_LOCAL_ROMS + " - skipped");
    return;
  }
  const char* dump = std::getenv("RN_FLASH_DUMP_DIR");
  std::vector<RomCase> cases = {
      // Start, run right with jumps; pause (Start) at 1500 for 2 s, resume.
      {"Super Mario Bros.", 3000,
       [](int f) -> uint8_t {
         if ((f >= 60 && f < 64) || (f >= 1500 && f < 1504) || (f >= 1620 && f < 1624)) return RN_BTN_START;
         if (f >= 1500 && f < 1624) return 0;
         uint8_t b = RN_BTN_RIGHT | RN_BTN_B;
         if (f % 50 < 18) b |= RN_BTN_A;
         return b;
       }},
      // Start, then shoot (auto) while weaving up and down.
      {"Gradius (Japan)", 6000,
       [](int f) -> uint8_t {
         if ((f >= 120 && f < 124) || (f >= 400 && f < 404)) return RN_BTN_START;
         uint8_t b = (f % 4 < 2) ? RN_BTN_B : 0;
         if (f % 8 < 4) b |= RN_BTN_A;
         if (f % 240 < 60) b |= RN_BTN_UP;
         else if (f % 240 >= 120 && f % 240 < 180) b |= RN_BTN_DOWN;
         if (f % 600 < 200) b |= RN_BTN_RIGHT;
         return b;
       }},
  };
  for (auto& c : cases) {
    std::string path;
    for (auto& n : names)
      if (n.rfind(c.name, 0) == 0) path = std::string(RN_LOCAL_ROMS) + "/" + n;
    if (path.empty()) continue;
    std::vector<uint8_t> rom;
    REQUIRE(fs::readFile(path, rom).ok());
    auto core = createCore(CoreKind::Nestopia);
    REQUIRE(core->loadROM(rom.data(), rom.size()).ok());
    FlashFilter std_(FlashLevel::Standard), low(FlashLevel::Low), high(FlashLevel::High);
    int alteredStd = 0, alteredLow = 0, alteredHigh = 0, dumped = 0;
    Frame out(NPX), outL(NPX), outH(NPX);
    for (int f = 0; f < c.frames; ++f) {
      REQUIRE(core->stepFrame(c.input(f), 0, true).ok());
      Frame in(core->video(), core->video() + NPX);
      uint64_t vh = core->videoHash();
      FlashFrameInfo info = std_.process(in.data(), out.data());
      if (low.process(in.data(), outL.data()).altered) ++alteredLow;
      if (high.process(in.data(), outH.data()).altered) ++alteredHigh;
      CHECK_EQ(core->videoHash(), vh);
      if (info.altered) ++alteredStd;
      if (dump && ((info.altered && dumped < 12) || f % 500 == 250 || f == 1560)) {
        if (info.altered) ++dumped;
        char p[512];
        std::snprintf(p, sizeof p, "%s/%s-%05d%s.ppm", dump, c.name, f, info.altered ? "-altered" : "");
        writePPM(p, in, out);
      }
    }
    MESSAGE(c.name << ": altered frames low/std/high = " << alteredLow << "/" << alteredStd << "/" << alteredHigh << " of "
                   << c.frames);
    // Normal play must be (almost) untouched: < 2% of the frames at the default level.
    CHECK(alteredStd * 50 < c.frames);
  }
}
