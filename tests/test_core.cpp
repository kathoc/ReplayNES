// Core-level tests: MockCore stepping, Nestopia integration, savestate round trips, concurrency.
#include <cstring>
#include <thread>

#include "replaynes/replaynes.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"
#include "util/Hash.h"

using namespace rn;
using namespace rntest;

TEST_CASE("mock: pause keeps frame, step advances exactly +1") {
  auto s = newSession(CoreKind::Mock);
  REQUIRE(s);
  CHECK_EQ(s->frame(), 0u);
  // "Paused": the frontend simply does not call step. Reading video/audio/hash never advances.
  for (int i = 0; i < 10; ++i) {
    (void)s->video();
    size_t n;
    (void)s->audio(&n);
    (void)s->stateHash();
  }
  CHECK_EQ(s->frame(), 0u);
  StepInfo info;
  REQUIRE(s->step(0, 0, 0, &info).ok());
  CHECK_EQ(s->frame(), 1u);
  CHECK_EQ(info.frame, 1u);
  for (int i = 0; i < 59; ++i) REQUIRE(s->step(1, 0, 0).ok());
  CHECK_EQ(s->frame(), 60u);
  CHECK_EQ(s->takeLength(), 60u);
}

TEST_CASE("mock: C API pause/step") {
  std::string d = tempDir("capi-mock");
  std::string rom = writeTestRom(d);
  rn_session_options o;
  rn_session_options_init(&o);
  o.core = RN_CORE_MOCK;
  rn_session* s = nullptr;
  REQUIRE_EQ(rn_session_new(rom.c_str(), nullptr, &o, &s), RN_OK);
  CHECK_EQ(rn_frame(s), 0u);
  uint64_t h = rn_state_hash(s);
  CHECK_EQ(rn_state_hash(s), h);
  CHECK_EQ(rn_frame(s), 0u);
  rn_step_info info;
  REQUIRE_EQ(rn_step(s, RN_BTN_A, 0, 0, &info), RN_OK);
  CHECK_EQ(rn_frame(s), 1u);
  CHECK_EQ(info.frame, 1u);
  size_t n = 0;
  rn_audio(s, &n);
  CHECK_EQ(n, size_t(rn_audio_samples_for_frame(0)));
  CHECK_EQ(rn_step(s, 0, 0, 0x80, &info), RN_ERR_INVALID_ARG);  // unknown event bit rejected
  CHECK(std::string(rn_last_error()).find("event") != std::string::npos);
  rn_session_close(s);
}

TEST_CASE("audio sample schedule is a pure function of the frame index") {
  uint64_t total = 0;
  for (uint64_t f = 0; f < 655171; f += 1) total += audioSamplesForFrame(f);
  // 655171 frames at 39375000/655171 fps = 39375000/ (fps) ... = exactly 655171*655171/39375000 s
  CHECK_EQ(total, audioSamplesBefore(655171));
  CHECK_EQ(audioSamplesBefore(39375000), uint64_t(48000) * 655171);  // 655171 s of audio
  for (uint64_t f = 0; f < 1000; ++f) {
    uint32_t n = audioSamplesForFrame(f);
    CHECK((n == 798 || n == 799));
  }
}

TEST_CASE("nestopia: test ROM runs, produces changing video and audible PCM") {
  auto core = createCore(CoreKind::Nestopia);
  auto rom = buildTestRom();
  REQUIRE(core->loadROM(rom.data(), rom.size()).ok());
  uint64_t prev = 0;
  int changes = 0, audible = 0;
  for (int f = 0; f < 300; ++f) {
    REQUIRE(core->stepFrame(uint8_t(f * 7), uint8_t(f * 3), true).ok());
    uint64_t v = core->videoHash();
    changes += v != prev;
    prev = v;
    size_t n;
    const int16_t* a = core->audio(&n);
    CHECK_EQ(n, size_t(audioSamplesForFrame(uint64_t(f))));
    for (size_t i = 0; i < n; ++i) if (a[i] != 0) { ++audible; break; }
  }
  CHECK(changes > 250);
  CHECK(audible > 250);
  CHECK((core->video()[0] >> 24) == 0xFFu);
  CHECK_EQ(core->frameIndex(), 300u);
}

TEST_CASE("nestopia: rejects garbage ROM with explicit error") {
  auto core = createCore(CoreKind::Nestopia);
  std::vector<uint8_t> junk(4096, 0x42);
  Status st = core->loadROM(junk.data(), junk.size());
  CHECK_FALSE(st.ok());
  CHECK_EQ(int(st.code), int(Err::RomInvalid));
}

static void saveRestoreScenario(CoreKind kind) {
  auto rom = buildTestRom();
  auto script = DeterminismHarness::randomScript(400, 77);
  script[250].events = kEvSoftReset;  // soft reset at the same frame in both runs
  script[330].events = kEvPowerCycle;
  auto core = createCore(kind);
  REQUIRE(core->loadROM(rom.data(), rom.size()).ok());
  for (int f = 0; f < 200; ++f) REQUIRE(core->stepRecord(script[f].p1, script[f].p2, script[f].events).ok());
  std::vector<uint8_t> st;
  REQUIRE(core->saveState(st).ok());
  std::vector<uint64_t> v1, s1;
  for (int f = 200; f < 400; ++f) {
    REQUIRE(core->stepRecord(script[f].p1, script[f].p2, script[f].events).ok());
    v1.push_back(core->videoHash());
    if (f % 10 == 9) s1.push_back(core->machineHash());
  }
  uint64_t end1 = core->machineHash();
  // Restore into the SAME instance and into a FRESH instance; both must match exactly.
  for (int pass = 0; pass < 2; ++pass) {
    std::unique_ptr<ICore> fresh;
    ICore* c = core.get();
    if (pass == 1) {
      fresh = createCore(kind);
      REQUIRE(fresh->loadROM(rom.data(), rom.size()).ok());
      c = fresh.get();
    }
    REQUIRE(c->loadState(st.data(), st.size()).ok());
    CHECK_EQ(c->frameIndex(), 200u);
    size_t k = 0, j = 0;
    for (int f = 200; f < 400; ++f) {
      REQUIRE(c->stepRecord(script[f].p1, script[f].p2, script[f].events).ok());
      CHECK_EQ(c->videoHash(), v1[k++]);
      if (f % 10 == 9) CHECK_EQ(c->machineHash(), s1[j++]);
    }
    CHECK_EQ(c->machineHash(), end1);
  }
}

TEST_CASE("nestopia: save -> 200f (soft reset + power cycle) -> restore -> same frames => identical hashes") {
  saveRestoreScenario(CoreKind::Nestopia);
}

TEST_CASE("mock: save/restore round trip") { saveRestoreScenario(CoreKind::Mock); }

TEST_CASE("nestopia: state from another core / truncated state is refused") {
  auto rom = buildTestRom();
  auto n = createCore(CoreKind::Nestopia), m = createCore(CoreKind::Mock);
  REQUIRE(n->loadROM(rom.data(), rom.size()).ok());
  REQUIRE(m->loadROM(rom.data(), rom.size()).ok());
  std::vector<uint8_t> ms, ns;
  REQUIRE(m->saveState(ms).ok());
  REQUIRE(n->saveState(ns).ok());
  CHECK_EQ(int(n->loadState(ms.data(), ms.size()).code), int(Err::CoreMismatch));
  CHECK_EQ(int(n->loadState(ns.data(), ns.size() / 2).code), int(Err::StateError));
}

TEST_CASE("nestopia: two instances run concurrently without interference") {
  auto rom = buildTestRom();
  auto scriptA = DeterminismHarness::randomScript(3000, 1, 700);
  auto scriptB = DeterminismHarness::randomScript(3000, 2, 900);
  Trace refA, refB;
  REQUIRE(DeterminismHarness::runFromStart(CoreKind::Nestopia, rom, scriptA, 50, refA).ok());
  REQUIRE(DeterminismHarness::runFromStart(CoreKind::Nestopia, rom, scriptB, 50, refB).ok());
  // Interleaved on one thread.
  {
    auto a = createCore(CoreKind::Nestopia), b = createCore(CoreKind::Nestopia);
    REQUIRE(a->loadROM(rom.data(), rom.size()).ok());
    REQUIRE(b->loadROM(rom.data(), rom.size()).ok());
    bool okA = true, okB = true;
    for (size_t f = 0; f < scriptA.size(); ++f) {
      a->stepRecord(scriptA[f].p1, scriptA[f].p2, scriptA[f].events);
      b->stepRecord(scriptB[f].p1, scriptB[f].p2, scriptB[f].events);
      okA = okA && a->videoHash() == refA.video[f];
      okB = okB && b->videoHash() == refB.video[f];
    }
    CHECK(okA);
    CHECK(okB);
  }
  // Truly parallel on 4 threads (2 per script).
  Trace t[4];
  std::thread th[4];
  for (int i = 0; i < 4; ++i)
    th[i] = std::thread([&, i] {
      DeterminismHarness::runFromStart(CoreKind::Nestopia, rom, i % 2 ? scriptB : scriptA, 50, t[i]);
    });
  for (auto& x : th) x.join();
  for (int i = 0; i < 4; ++i) {
    Divergence d = DeterminismHarness::compare(i % 2 ? refB : refA, t[i], true);
    CHECK_FALSE(d.found);
    if (d.found) MESSAGE("thread " << i << ": " << d.detail);
  }
}

TEST_CASE("nestopia: power-on state does not depend on heap garbage (uninitialised members)") {
  // A destroyed, fully used instance leaves its memory behind; the next instance usually reuses
  // it. Without patch 4 (Triangle::linearCtrl) this changes the power-on machine state.
  auto rom = buildTestRom();
  auto script = DeterminismHarness::randomScript(300, 31, 0, 5);
  auto trace = [&]() {
    auto c = createCore(CoreKind::Nestopia);
    c->loadROM(rom.data(), rom.size());
    Hasher64 h;
    h.u64(c->machineHash());
    for (int i = 0; i < 5; ++i) {
      c->stepRecord(script[i].p1, script[i].p2, 0);
      h.u64(c->machineHash());
    }
    return h.digest();
  };
  uint64_t first = trace();
  for (int round = 0; round < 3; ++round) {
    {
      auto dirty = createCore(CoreKind::Nestopia);
      REQUIRE(dirty->loadROM(rom.data(), rom.size()).ok());
      for (auto& r : script) dirty->stepRecord(r.p1, r.p2, r.events);
    }
    CHECK_EQ(trace(), first);
  }
}

TEST_CASE("nestopia: reloading the ROM in a used instance equals a fresh power-on") {
  auto rom = buildTestRom();
  auto script = DeterminismHarness::randomScript(900, 3, 0, 5);
  auto fresh = createCore(CoreKind::Nestopia);
  REQUIRE(fresh->loadROM(rom.data(), rom.size()).ok());
  auto used = createCore(CoreKind::Nestopia);
  REQUIRE(used->loadROM(rom.data(), rom.size()).ok());
  for (auto& r : script) used->stepRecord(r.p1, r.p2, r.events);
  REQUIRE(used->loadROM(rom.data(), rom.size()).ok());
  CHECK_EQ(used->machineHash(), fresh->machineHash());
  for (int i = 0; i < 5; ++i) {
    fresh->stepRecord(script[i].p1, script[i].p2, 0);
    used->stepRecord(script[i].p1, script[i].p2, 0);
    CHECK_EQ(used->machineHash(), fresh->machineHash());
  }
}

TEST_CASE("generated test ROM is stable and NROM") {
  auto a = buildTestRom(), b = buildTestRom();
  CHECK(a == b);
  CHECK_EQ(a.size(), size_t(16 + 16384 + 8192));
  CHECK_EQ(int(a[4]), 1);
  CHECK_EQ(int(a[5]), 1);
  CHECK_EQ(int(a[6] >> 4), 0);  // mapper 0
}
