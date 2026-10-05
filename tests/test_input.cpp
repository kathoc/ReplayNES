// Input pipeline: remap, turbo period/duty, SOCD, hotkey separation, taps, analog, disconnect,
// and replay independence from physical devices.
#include "input/InputPipeline.h"
#include "replaynes/replaynes.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"

using namespace rn;
using namespace rntest;

static uint8_t p1At(InputPipeline& p, uint64_t f) {
  uint8_t a, b;
  p.sampleGame(f, a, b);
  return a;
}

TEST_CASE("input: remap, multiple bindings, unbind") {
  InputPipeline p;
  REQUIRE(p.bind("kb:6", "p1.a").ok());
  REQUIRE(p.bind("gc0:buttonA", "p1.a").ok());
  REQUIRE(p.bind("gc0:buttonB", "p2.b").ok());
  CHECK_EQ(int(p.bind("kb:1", "p3.a").code), int(Err::InvalidArg));
  p.setPressed("gc0:buttonA", true);
  p.setPressed("gc0:buttonB", true);
  uint8_t a, b;
  p.sampleGame(0, a, b);
  CHECK_EQ(int(a), RN_BTN_A);
  CHECK_EQ(int(b), RN_BTN_B);
  REQUIRE(p.unbind("gc0:buttonA", "").ok());
  p.sampleGame(1, a, b);
  CHECK_EQ(int(a), 0);
}

TEST_CASE("input: turbo period/duty is frame-exact and phase starts at press") {
  InputPipeline p;
  REQUIRE(p.bind("kb:t", "p1.turbo_a").ok());
  REQUIRE(p.setTurbo(4, 2).ok());
  CHECK_EQ(int(p.setTurbo(2, 3).code), int(Err::InvalidArg));
  CHECK_EQ(int(p.setTurbo(0, 0).code), int(Err::InvalidArg));
  CHECK_EQ(int(p1At(p, 99)), 0);
  p.setPressed("kb:t", true);
  std::string pat;
  for (uint64_t f = 100; f < 112; ++f) pat += (p1At(p, f) & RN_BTN_A) ? '1' : '0';
  CHECK_EQ(pat, std::string("110011001100"));
  p.setPressed("kb:t", false);
  CHECK_EQ(int(p1At(p, 112)), 0);
  REQUIRE(p.setTurbo(3, 1).ok());
  p.setPressed("kb:t", true);
  pat.clear();
  for (uint64_t f = 200; f < 209; ++f) pat += (p1At(p, f) & RN_BTN_A) ? '1' : '0';
  CHECK_EQ(pat, std::string("100100100"));
  // Turbo B on P2 maps to the B bit of player 2.
  REQUIRE(p.bind("kb:y", "p2.turbo_b").ok());
  p.setPressed("kb:y", true);
  uint8_t a, b;
  p.sampleGame(300, a, b);
  CHECK_EQ(int(b), RN_BTN_B);
}

TEST_CASE("input: SOCD policies") {
  InputPipeline p;
  p.bind("l", "p1.left");
  p.bind("r", "p1.right");
  p.bind("u", "p1.up");
  p.bind("d", "p1.down");
  p.setPressed("l", true);
  p.setPressed("r", true);  // pressed after left
  p.setPressed("u", true);
  p.setSocd(Socd::Neutral);
  CHECK_EQ(int(p1At(p, 0)), RN_BTN_UP);
  p.setSocd(Socd::LastWins);
  CHECK_EQ(int(p1At(p, 1)), RN_BTN_UP | RN_BTN_RIGHT);
  p.setPressed("r", false);
  p.setPressed("r", true);
  p.setPressed("l", false);
  p.setPressed("l", true);  // left is now the latest
  CHECK_EQ(int(p1At(p, 2)), RN_BTN_UP | RN_BTN_LEFT);
  p.setSocd(Socd::Allow);
  CHECK_EQ(int(p1At(p, 3)), RN_BTN_UP | RN_BTN_LEFT | RN_BTN_RIGHT);
  p.setPressed("d", true);
  p.setSocd(Socd::Neutral);
  CHECK_EQ(int(p1At(p, 4)), 0);
}

TEST_CASE("input: hotkeys never reach the game bitfield") {
  InputPipeline p;
  REQUIRE(p.bind("kb:p", "hk.pause").ok());
  REQUIRE(p.bind("kb:r", "hk.soft_reset").ok());
  REQUIRE(p.bind("kb:x", "p1.a").ok());
  REQUIRE(p.bind("kb:x", "hk.bookmark").ok());  // one key may drive both; the hotkey part stays separate
  p.setPressed("kb:p", true);
  p.setPressed("kb:r", true);
  uint8_t a, b;
  p.sampleGame(0, a, b);
  CHECK_EQ(int(a), 0);
  CHECK_EQ(int(b), 0);
  uint32_t edges, held;
  p.pollHotkeys(edges, held);
  CHECK_EQ(edges, uint32_t(RN_HK_PAUSE | RN_HK_SOFT_RESET));
  CHECK_EQ(held, uint32_t(RN_HK_PAUSE | RN_HK_SOFT_RESET));
  p.pollHotkeys(edges, held);
  CHECK_EQ(edges, 0u);  // edges are reported once
  p.setPressed("kb:x", true);
  p.sampleGame(1, a, b);
  CHECK_EQ(int(a), RN_BTN_A);
  p.pollHotkeys(edges, held);
  CHECK_EQ(edges, uint32_t(RN_HK_BOOKMARK));
}

TEST_CASE("input: sub-frame taps are latched for exactly one sample") {
  InputPipeline p;
  p.bind("kb:z", "p1.b");
  p.setPressed("kb:z", true);
  p.setPressed("kb:z", false);
  CHECK_EQ(int(p1At(p, 0)), RN_BTN_B);
  CHECK_EQ(int(p1At(p, 1)), 0);
}

TEST_CASE("input: analog threshold, controller disconnect releases buttons") {
  InputPipeline p;
  p.bind("gc0:lstick.left", "p1.left");
  p.bind("gc0:lstick.up", "p1.up");
  p.bind("gc0:buttonA", "p1.a");
  p.bind("kb:s", "p1.start");
  REQUIRE(p.setAnalogThreshold(0.6f).ok());
  p.setAxis("gc0:lstick", -0.5f, 0.0f);
  CHECK_EQ(int(p1At(p, 0)), 0);
  p.setAxis("gc0:lstick", -0.7f, 0.65f);
  CHECK_EQ(int(p1At(p, 1)), RN_BTN_LEFT | RN_BTN_UP);
  p.setPressed("gc0:buttonA", true);
  p.setPressed("kb:s", true);
  CHECK_EQ(int(p1At(p, 2)), RN_BTN_LEFT | RN_BTN_UP | RN_BTN_A | RN_BTN_START);
  p.releasePrefix("gc0:");
  CHECK_EQ(int(p1At(p, 3)), RN_BTN_START);
}

TEST_CASE("input: JSON round trip; invalid config rejected without partial changes") {
  InputPipeline p;
  p.bind("kb:6", "p1.a");
  p.bind("kb:p", "hk.pause");
  p.setTurbo(6, 2);
  p.setSocd(Socd::LastWins);
  std::string j = p.toJson();
  InputPipeline q;
  REQUIRE(q.fromJson(j).ok());
  CHECK_EQ(q.toJson(), j);
  CHECK_FALSE(q.fromJson("{\"version\":1,\"bindings\":[{\"input\":\"x\",\"action\":\"nope\"}]}").ok());
  CHECK_FALSE(q.fromJson("not json").ok());
  CHECK_EQ(q.toJson(), j);
}

TEST_CASE("input: recorded take replays identically with or without physical devices") {
  // Record through the pipeline (turbo + held buttons + taps + SOCD) ...
  InputPipeline p;
  p.bind("kb:t", "p1.turbo_a");
  p.bind("kb:l", "p1.left");
  p.bind("kb:r", "p1.right");
  p.bind("gc0:b", "p2.b");
  p.setTurbo(3, 1);
  auto s = newSession(CoreKind::Nestopia);
  std::vector<uint64_t> hashes;
  std::vector<uint8_t> expectedP1;
  for (uint64_t f = 0; f < 900; ++f) {
    p.setPressed("kb:t", (f / 50) % 2 == 0);
    p.setPressed("kb:l", (f / 70) % 2 == 0);
    p.setPressed("kb:r", (f / 30) % 3 == 0);
    p.setPressed("gc0:b", (f / 90) % 2 == 1);
    if (f == 400) p.releasePrefix("gc0:");  // controller disconnect mid-take
    uint8_t a, b;
    p.sampleGame(s->frame(), a, b);
    expectedP1.push_back(a);
    REQUIRE(s->step(a, b, 0).ok());
    hashes.push_back(s->stateHash());
  }
  // ... the log holds the turbo-converted bitfields, not the turbo setting.
  auto log = s->timeline().flattenActive();
  for (size_t i = 0; i < log.size(); ++i) CHECK_EQ(int(log[i].p1), int(expectedP1[i]));
  // Replay without any devices (zero input) and with a hostile pipeline pressing everything.
  InputPipeline hostile;
  hostile.bind("x", "p1.a");
  hostile.bind("y", "p2.start");
  hostile.setPressed("x", true);
  hostile.setPressed("y", true);
  for (int variant = 0; variant < 2; ++variant) {
    auto r = newSession(CoreKind::Nestopia);
    for (auto& rec : log) REQUIRE(r->step(rec.p1, rec.p2, rec.events).ok());
    REQUIRE(r->seek(0).ok());
    r->setMode(Mode::Replay);
    bool same = true;
    for (size_t i = 0; i < log.size(); ++i) {
      uint8_t a = 0, b = 0;
      if (variant == 1) hostile.sampleGame(i, a, b);
      REQUIRE(r->step(a, b, kEvSoftReset).ok());
      same = same && r->stateHash() == hashes[i];
    }
    CHECK(same);
  }
}
