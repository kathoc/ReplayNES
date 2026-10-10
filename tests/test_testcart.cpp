// ReplayNES Test Cartridge (tools/testcart): ROM image, determinism with scripted input on every
// screen, the raster effects (palette chart, split scroll) and the rapid-fire meter's numbers.
#include <cstring>
#include <functional>

#include "replaynes/replaynes.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"
#include "testrom/TestCartridge.h"

using namespace rn;
using namespace rntest;
namespace tc = rn::testcart;

namespace {

constexpr uint8_t A = RN_BTN_A, B = RN_BTN_B, DOWN = RN_BTN_DOWN, LEFT = RN_BTN_LEFT, START = RN_BTN_START;

struct Cart {
  std::unique_ptr<ICore> core = createCore(CoreKind::Nestopia);
  Cart() {
    auto rom = buildTestCartridge();
    REQUIRE(core->loadROM(rom.data(), rom.size()).ok());
  }
  void step(uint8_t p1 = 0, int n = 1, uint8_t p2 = 0) {
    for (int i = 0; i < n; ++i) REQUIRE(core->stepFrame(p1, p2, true).ok());
  }
  uint8_t ram(unsigned addr) const { return core->cpuRam()[addr]; }
  // 16-bit value of button x stored as lo[2] / hi[2] arrays.
  unsigned v16(unsigned lo, unsigned hi, int x) const { return ram(lo + x) | (ram(hi + x) << 8); }
  void tap(uint8_t btn) {
    step(btn, 2);
    step(0, 4);
  }
  // From the title: Down (screen - 1) times, then A.
  void open(unsigned screen) {
    step(0, 30);
    for (unsigned i = 1; i < screen; ++i) tap(DOWN);
    tap(A);
    step(0, 20);
    REQUIRE_EQ(unsigned(ram(tc::screen)), screen);
  }
};

// Every frame of `pattern(i)` (i = 0..frames-1) on pad 1, then `tail` idle frames. A 10-second
// test starts with the first press and ends 599 frames later; presses after its cool-down would
// start a new test, so patterns stop at 600 frames.
void play(Cart& c, int frames, const std::function<uint8_t(int)>& pattern, int tail = 700) {
  for (int i = 0; i < frames; ++i) c.step(pattern(i));
  c.step(0, tail);
}

}  // namespace

TEST_CASE("test cartridge: iNES NROM-128 image, own code, legacy test ROM unchanged") {
  auto rom = buildTestCartridge();
  REQUIRE_EQ(rom.size(), size_t(16 + 16384 + 8192));
  CHECK(std::memcmp(rom.data(), "NES\x1A", 4) == 0);
  CHECK_EQ(int(rom[4]), 1);  // 16 KiB PRG
  CHECK_EQ(int(rom[5]), 1);  // 8 KiB CHR
  CHECK_EQ(int(rom[6]), 1);  // mapper 0, vertical mirroring
  CHECK_EQ(int(rom[7]), 0);
  // The determinism fixture is a different ROM and stays as it was.
  CHECK(buildTestRom() != rom);
  CHECK_EQ(buildTestRom().size(), size_t(16 + 16384 + 8192));
}

TEST_CASE("test cartridge: C API writes the same image") {
  std::string dir = tempDir("testcart");
  std::string p = fs::join(dir, "cart.nes");
  REQUIRE(rn_write_test_cartridge(p.c_str()) == RN_OK);
  std::vector<uint8_t> f;
  REQUIRE(fs::readFile(p, f).ok());
  CHECK(f == buildTestCartridge());
  CHECK(rn_write_test_cartridge(nullptr) != RN_OK);
}

// Scripted tour: every screen with input on it (menu, toggles, sprite moves, scroll modes, pads,
// meter taps, sound), plus a soft reset. Run twice and once from a mid-way state load.
static std::vector<InputRecord> tourScript() {
  std::vector<InputRecord> s;
  auto add = [&](uint8_t p1, int n, uint8_t p2 = 0, uint8_t ev = 0) {
    for (int i = 0; i < n; ++i) s.push_back({p1, p2, uint8_t(i == 0 ? ev : 0)});
  };
  auto tap = [&](uint8_t b) { add(b, 2); add(0, 4); };
  add(0, 40);
  tap(A);                                 // palette chart
  add(0, 30); tap(A); add(0, 20); tap(B); add(0, 20);
  tap(RN_BTN_SELECT);                     // colour bars
  add(0, 20); tap(A); tap(A); tap(B); add(0, 20);
  tap(RN_BTN_SELECT);                     // sprites
  add(RN_BTN_RIGHT, 40); add(RN_BTN_UP, 30); tap(A); add(B | A, 2); add(0, 60);
  tap(RN_BTN_SELECT);                     // scrolling: all modes, split with D-pad
  add(0, 60); tap(A); add(0, 30); tap(A); add(0, 30); tap(A); add(RN_BTN_RIGHT | DOWN, 20); tap(A);
  add(RN_BTN_LEFT, 30); add(RN_BTN_UP, 20); tap(B); add(0, 30);
  tap(RN_BTN_SELECT);                     // controllers
  add(0x55, 10, 0xAA); add(0xF0, 10, 0x0F); add(0, 10);
  add(RN_BTN_SELECT | START, 3);          // back to the menu
  add(0, 20);
  for (int i = 0; i < 5; ++i) tap(DOWN);
  tap(A);                                 // rapid-fire meter: taps on both buttons
  add(0, 20);
  for (int i = 0; i < 100; ++i) add(uint8_t((i % 2 ? 0 : A) | (i % 3 ? 0 : B)), 1);
  add(0, 120);
  tap(LEFT);                              // turbo check
  add(0, 20);
  for (int i = 0; i < 90; ++i) add(uint8_t(i % 4 < 2 ? A : 0), 1);
  tap(RN_BTN_SELECT);                     // sound test
  add(0, 20); tap(A); tap(DOWN); tap(A); tap(RN_BTN_RIGHT); tap(DOWN); tap(A); tap(DOWN); tap(A); tap(B);
  tap(DOWN); tap(A); add(0, 60);
  add(0, 1, 0, kEvSoftReset);             // soft reset: best records survive, screen restarts
  add(0, 120);
  return s;
}

TEST_CASE("test cartridge: scripted tour is deterministic (2 runs + mid-way state load)") {
  auto rom = buildTestCartridge();
  auto script = tourScript();
  HarnessConfig cfg;
  cfg.core = CoreKind::Nestopia;
  cfg.runs = 2;
  cfg.stateEvery = 10;
  cfg.midFrame = script.size() / 2;
  HarnessReport rep;
  REQUIRE(DeterminismHarness::run(cfg, rom, script, rep).ok());
  CHECK_FALSE(rep.runs.found);
  if (rep.runs.found) MESSAGE(rep.runs.detail);
  CHECK_FALSE(rep.mid.found);
  if (rep.mid.found) MESSAGE(rep.mid.detail);
}

TEST_CASE("test cartridge: random input with resets is deterministic") {
  auto rom = buildTestCartridge();
  auto script = DeterminismHarness::randomScript(6000, 77, 1200, 5);
  HarnessConfig cfg;
  cfg.core = CoreKind::Nestopia;
  cfg.runs = 2;
  cfg.stateEvery = 50;
  cfg.midFrame = 3001;
  HarnessReport rep;
  REQUIRE(DeterminismHarness::run(cfg, rom, script, rep).ok());
  CHECK_FALSE(rep.runs.found);
  CHECK_FALSE(rep.mid.found);
}

TEST_CASE("test cartridge: palette chart shows all 64 colours, stable from frame to frame") {
  Cart c;
  c.open(tc::SCR_PALETTE);
  uint64_t first = 0;
  for (int f = 0; f < 60; ++f) {
    c.step();
    if (f == 0) first = c.core->videoHash();
    CHECK_EQ(c.core->videoHash(), first);  // no frame-to-frame jitter of the raster kernel
  }
  uint32_t phase = 0;
  uint64_t fr = 0;
  const uint16_t* codes = c.core->videoCodes(&phase, &fr);
  REQUIRE(codes != nullptr);
  // Row k (hue k) starts at line 28 + 12k: 7 lines of colour, then 5 black lines. Swatches at
  // x = 24..71, 80..127, 136..183, 192..239 hold $0k, $1k, $2k, $3k.
  const int x0[4] = {24, 80, 136, 192};
  for (int hue = 0; hue < 16; ++hue) {
    for (int lum = 0; lum < 4; ++lum) {
      const int want = lum * 16 + hue;
      for (int dy = 0; dy < 7; ++dy) {
        int y = 28 + 12 * hue + dy;
        for (int x = x0[lum]; x < x0[lum] + 48; x += 7) CHECK_EQ(int(codes[y * 256 + x] & 0x3F), want);
      }
      for (int dy = 7; dy < 12; ++dy) {  // gap: rendering off, only black (no palette-write stripes)
        int y = 28 + 12 * hue + dy;
        for (int x = 0; x < 256; ++x) CHECK_EQ(int(codes[y * 256 + x] & 0x3F), 0x0F);
      }
    }
  }
  // A: emphasis bits change the code's emphasis field everywhere, the chart stays put.
  c.tap(A);
  c.step(0, 5);
  codes = c.core->videoCodes(&phase, &fr);
  CHECK_EQ(int(codes[(28 + 5) * 256 + 100] & 0x3F), 0x10);
  CHECK_EQ(int(codes[(28 + 5) * 256 + 100] >> 6), 1);  // red emphasis
}

TEST_CASE("test cartridge: split scroll is stable and matches the unsplit playfield") {
  // Same position in D-pad mode (3) and split mode (4): the split's playfield (from line 32) must
  // equal the unsplit picture's top rows.
  auto shoot = [](bool split) {
    Cart c;
    c.open(tc::SCR_SCROLL);
    for (int i = 0; i < 3; ++i) c.tap(A);  // H, V, diagonal -> D-pad
    if (split) c.tap(A);
    else c.step(0, 6);
    c.step(RN_BTN_RIGHT, 37);
    c.step(DOWN, 13);
    c.step(0, 10);
    std::vector<std::vector<uint32_t>> frames;
    for (int f = 0; f < 30; ++f) {
      c.step();
      frames.emplace_back(c.core->video(), c.core->video() + 256 * 240);
    }
    return frames;
  };
  auto plain = shoot(false), split = shoot(true);
  for (auto& f : split) CHECK(f == split[0]);
  bool same = true;
  for (int y = 0; y < 150; ++y)
    same = same && std::memcmp(&split[0][(32 + y) * 256], &plain[0][y * 256], 256 * 4) == 0;
  CHECK(same);
}

// ---------------------------------------------------------------------------- rapid-fire meter
TEST_CASE("rapid-fire meter: alternating every frame for 60 frames = 59 edges, 30 presses per 60 samples") {
  Cart c;
  c.open(tc::SCR_METER);
  play(c, 60, [](int i) { return uint8_t(i % 2 == 0 ? A : 0); });
  CHECK_EQ(int(c.ram(tc::m_peake)), 59);  // 60 samples: at most 59 transitions
  CHECK_EQ(int(c.ram(tc::m_peakp)), 30);
  CHECK_EQ(c.v16(tc::m_cnt_lo, tc::m_cnt_hi, 0), 30u);
  CHECK_EQ(c.v16(tc::m_ecnt_lo, tc::m_ecnt_hi, 0), 60u);  // + the release after the last press
}

TEST_CASE("rapid-fire meter: press every 2 frames for 10 s = 30.0/s, 599 edges (fencepost)") {
  Cart c;
  c.open(tc::SCR_METER);
  play(c, 600, [](int i) { return uint8_t(i % 2 == 0 ? A : 0); });
  CHECK_EQ(c.v16(tc::m_cnt_lo, tc::m_cnt_hi, 0), 300u);
  CHECK_EQ(c.v16(tc::m_ecnt_lo, tc::m_ecnt_hi, 0), 599u);  // 600 samples -> 599 transitions
  CHECK_EQ(c.v16(tc::r_rate_lo, tc::r_rate_hi, 0), 300u);  // 30.0 presses/s
  CHECK_EQ(int(c.ram(tc::m_peakp)), 30);
  CHECK_EQ(int(c.ram(tc::m_peake)), 59);
  CHECK_EQ(c.v16(tc::r_ppavg_lo, tc::r_ppavg_hi, 0), 200u);  // 2.00 frames
  CHECK_EQ(c.v16(tc::r_ppsd_lo, tc::r_ppsd_hi, 0), 0u);
  CHECK_EQ(c.v16(tc::r_rravg_lo, tc::r_rravg_hi, 0), 200u);
  CHECK_EQ(c.v16(tc::r_hdavg_lo, tc::r_hdavg_hi, 0), 100u);  // held 1 frame
  CHECK_EQ(c.v16(tc::r_gpavg_lo, tc::r_gpavg_hi, 0), 100u);  // released 1 frame
  CHECK_EQ(int(c.ram(tc::r_duty)), 50);
  CHECK_EQ(int(c.ram(tc::m_minpp)), 2);
  CHECK_EQ(c.v16(tc::m_hist_lo, tc::m_hist_hi, 0), 299u);  // all intervals in the 2-frame bin
  CHECK_EQ(int(c.ram(tc::best_a)) | (c.ram(tc::best_a + 1) << 8), 300);
  CHECK_EQ(int(c.ram(tc::best_e)) | (c.ram(tc::best_e + 1) << 8), 599);
}

TEST_CASE("rapid-fire meter: A every 4 frames (2/2), B every 3 frames (1/2) at the same time") {
  Cart c;
  c.open(tc::SCR_METER);
  play(c, 600, [](int i) { return uint8_t((i % 4 < 2 ? A : 0) | (i % 3 == 0 ? B : 0)); });
  CHECK_EQ(c.v16(tc::m_cnt_lo, tc::m_cnt_hi, 0), 150u);
  CHECK_EQ(c.v16(tc::r_rate_lo, tc::r_rate_hi, 0), 150u);   // 15.0/s
  CHECK_EQ(c.v16(tc::r_ppavg_lo, tc::r_ppavg_hi, 0), 400u);
  CHECK_EQ(c.v16(tc::r_hdavg_lo, tc::r_hdavg_hi, 0), 200u);
  CHECK_EQ(int(c.ram(tc::r_duty)), 50);
  CHECK_EQ(c.v16(tc::m_cnt_lo, tc::m_cnt_hi, 1), 200u);     // B: 20.0/s
  CHECK_EQ(c.v16(tc::r_rate_lo, tc::r_rate_hi, 1), 200u);
  CHECK_EQ(c.v16(tc::r_ppavg_lo, tc::r_ppavg_hi, 1), 300u);
  CHECK_EQ(c.v16(tc::r_hdavg_lo, tc::r_hdavg_hi, 1), 100u);
  CHECK_EQ(c.v16(tc::r_gpavg_lo, tc::r_gpavg_hi, 1), 200u);
  CHECK_EQ(int(c.ram(tc::r_duty + 1)), 33);
  CHECK_EQ(int(c.ram(tc::m_peakp + 1)), 20);
}

TEST_CASE("rapid-fire meter: intervals 3, 5, 3, 5 ... = mean 4.00, deviation 1.00") {
  Cart c;
  c.open(tc::SCR_METER);
  // presses at 0, 3, 8, 11, 16, ... 592 (each held 1 frame): 149 presses, 74 + 74 intervals
  play(c, 593, [](int i) { int m = i % 8; return uint8_t(m == 0 || m == 3 ? A : 0); });
  CHECK_EQ(c.v16(tc::r_ppavg_lo, tc::r_ppavg_hi, 0), 400u);
  CHECK_EQ(c.v16(tc::r_ppsd_lo, tc::r_ppsd_hi, 0), 100u);
  CHECK_EQ(c.v16(tc::r_rravg_lo, tc::r_rravg_hi, 0), 400u);
  CHECK_EQ(c.v16(tc::r_rrsd_lo, tc::r_rrsd_hi, 0), 100u);
  CHECK_EQ(int(c.ram(tc::m_minpp)), 3);
}

TEST_CASE("rapid-fire meter: turbo check measures the input pipeline's turbo period and duty") {
  struct Case { uint32_t period, duty; };
  for (Case k : {Case{2, 1}, Case{3, 1}, Case{4, 2}, Case{6, 4}, Case{8, 1}}) {
    Cart c;
    c.open(tc::SCR_METER);
    c.tap(LEFT);  // turbo check
    c.step(0, 20);
    REQUIRE_EQ(int(c.ram(tc::m_mode)), 1);
    rn_input* in = rn_input_new();
    REQUIRE(rn_input_bind(in, "kb:1", "p1.turbo_a") == RN_OK);
    REQUIRE(rn_input_set_turbo(in, k.period, k.duty) == RN_OK);
    rn_input_set_pressed(in, "kb:1", 1);
    for (uint64_t f = 0; f < 120; ++f) {
      uint8_t p1 = 0, p2 = 0;
      rn_input_sample_game(in, f, &p1, &p2);
      c.step(p1, 1, p2);
    }
    rn_input_free(in);
    CHECK_EQ(unsigned(c.ram(tc::t_per)), k.period);
    CHECK_EQ(unsigned(c.ram(tc::t_hold)), k.duty);
    CHECK_EQ(unsigned(c.ram(tc::t_gap)), k.period - k.duty);
    CHECK_EQ(unsigned(c.ram(tc::t_duty)), (k.duty * 100 + k.period / 2) / k.period);
    CHECK_EQ(unsigned(c.ram(tc::t_min)), k.period);
    CHECK_EQ(unsigned(c.ram(tc::t_max)), k.period);
  }
}

TEST_CASE("rapid-fire meter: best record survives a soft reset, not a power cycle") {
  Cart c;
  c.open(tc::SCR_METER);
  play(c, 600, [](int i) { return uint8_t(i % 4 == 0 ? A : 0); });
  CHECK_EQ(int(c.ram(tc::best_a)), 150);
  REQUIRE(c.core->softReset().ok());
  c.step(0, 30);
  CHECK_EQ(int(c.ram(tc::best_a)), 150);
  REQUIRE(c.core->powerCycle().ok());
  c.step(0, 30);
  CHECK_EQ(int(c.ram(tc::best_a)), 0);
}
