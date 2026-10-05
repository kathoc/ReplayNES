// Determinism harness tests (>= 10000 frames on the real core).
#include "support/TestUtil.h"
#include "support/rn_test.h"

using namespace rn;
using namespace rntest;

TEST_CASE("harness: 10000 frames, 3 runs + mid-savestate run, no divergence (nestopia)") {
  auto rom = buildTestRom();
  auto script = DeterminismHarness::randomScript(10000, 2024, 1500, 5);
  HarnessConfig cfg;
  cfg.core = CoreKind::Nestopia;
  cfg.runs = 3;
  cfg.stateEvery = 50;
  cfg.midFrame = 5003;
  HarnessReport rep;
  REQUIRE(DeterminismHarness::run(cfg, rom, script, rep).ok());
  CHECK_FALSE(rep.runs.found);
  if (rep.runs.found) MESSAGE(rep.runs.detail);
  CHECK_FALSE(rep.mid.found);  // machine state + video + audio bit-identical after a state load
  if (rep.mid.found) MESSAGE(rep.mid.detail);
  MESSAGE(rep.frames << " frames in " << rep.seconds << " s");
}

TEST_CASE("harness: injected divergence is reported at the right frame and component") {
  auto rom = buildTestRom();
  auto script = DeterminismHarness::randomScript(6000, 9, 0, 5);
  HarnessConfig cfg;
  cfg.core = CoreKind::Nestopia;
  cfg.runs = 2;
  cfg.stateEvery = 100;
  HarnessReport rep;
  const uint64_t injectAt = 4321;
  REQUIRE(DeterminismHarness::run(cfg, rom, script, rep, [&](uint64_t f, InputRecord& r) {
            if (f == injectAt) r.p1 ^= 0x01;  // flip A for one frame
          }).ok());
  REQUIRE(rep.runs.found);
  CHECK(rep.runs.frame >= injectAt);
  CHECK(rep.runs.frame <= injectAt + 1);
  CHECK((rep.runs.component == "video" || rep.runs.component == "state" || rep.runs.component == "audio"));
  MESSAGE(rep.runs.detail);
}

TEST_CASE("harness: hidden state divergence (no visible change) is caught by the state hash") {
  // Mock core: perturb only an input bit that changes the accumulator; the comparator must
  // report it, and compare() must flag state-only differences too.
  Trace a, b;
  a.first = b.first = 0;
  for (int i = 0; i < 100; ++i) { a.video.push_back(i); a.audio.push_back(i); b.video.push_back(i); b.audio.push_back(i); }
  a.state[50] = 1;
  b.state[50] = 2;
  Divergence d = DeterminismHarness::compare(a, b, true);
  REQUIRE(d.found);
  CHECK_EQ(d.component, std::string("state"));
  CHECK_EQ(d.frame, 49u);
}

TEST_CASE("harness: PCM after a state load is bit-identical (patched APU output stage)") {
  auto rom = buildTestRom();
  auto script = DeterminismHarness::randomScript(3000, 5, 0, 5);
  for (uint64_t mid : {uint64_t(1), uint64_t(997), uint64_t(1000), uint64_t(2345)}) {
    HarnessConfig cfg;
    cfg.core = CoreKind::Nestopia;
    cfg.runs = 1;
    cfg.stateEvery = 1;  // machine state compared after every frame
    cfg.midFrame = mid;
    cfg.compareAudioAfterLoad = true;
    HarnessReport rep;
    REQUIRE(DeterminismHarness::run(cfg, rom, script, rep).ok());
    CHECK_FALSE(rep.mid.found);
    if (rep.mid.found) MESSAGE("mid=" << mid << ": " << rep.mid.detail);
  }
}

TEST_CASE("harness: mock core 20000 frames strict incl. audio after load") {
  auto rom = buildTestRom();
  auto script = DeterminismHarness::randomScript(20000, 3, 2000, 3);
  HarnessConfig cfg;
  cfg.core = CoreKind::Mock;
  cfg.runs = 3;
  cfg.midFrame = 12345;
  cfg.compareAudioAfterLoad = true;
  HarnessReport rep;
  REQUIRE(DeterminismHarness::run(cfg, rom, script, rep).ok());
  CHECK_FALSE(rep.runs.found);
  CHECK_FALSE(rep.mid.found);
}
