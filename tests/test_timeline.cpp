// Record/replay, branching, takes/undo, seek correctness, rewind/branch stress.
#include <algorithm>
#include <random>

#include "support/TestUtil.h"
#include "support/rn_test.h"

using namespace rn;
using namespace rntest;

TEST_CASE("RLE: round trip and rejection of malformed data") {
  std::vector<InputRecord> recs;
  for (int i = 0; i < 1000; ++i) recs.push_back({uint8_t(i / 37), uint8_t(i / 100), uint8_t(i % 250 == 0 ? 1 : 0)});
  std::vector<uint8_t> enc;
  encodeRecords(recs.data(), recs.size(), enc);
  CHECK(enc.size() < recs.size());
  std::vector<InputRecord> dec;
  REQUIRE(decodeRecords(enc.data(), enc.size(), recs.size(), dec));
  CHECK(dec == recs);
  CHECK_FALSE(decodeRecords(enc.data(), enc.size(), recs.size() + 1, dec));  // count mismatch
  CHECK_FALSE(decodeRecords(enc.data(), enc.size() - 1, recs.size(), dec));  // truncated
  std::vector<uint8_t> bad = {1, 0, 0, 0x80};  // unknown event bit
  CHECK_FALSE(decodeRecords(bad.data(), bad.size(), 1, dec));
}

TEST_CASE("record -> fresh instance replay gives identical hashes (nestopia)") {
  auto s = newSession(CoreKind::Nestopia);
  REQUIRE(s);
  auto script = DeterminismHarness::randomScript(3000, 11, 900, 4);
  std::vector<uint64_t> v, a;
  for (auto& r : script) {
    REQUIRE(s->step(r.p1, r.p2, r.events).ok());
    v.push_back(s->videoHash());
    a.push_back(s->audioHash());
  }
  uint64_t end = s->stateHash();
  // Canonical log = the final bitfields actually applied.
  std::vector<InputRecord> log = s->timeline().flattenActive();
  REQUIRE(log == script);
  Replay r = straightReplay(CoreKind::Nestopia, log);
  CHECK(r.video == v);
  CHECK(r.audio == a);  // both runs start at power-on: audio is bit-identical
  CHECK_EQ(r.finalState, end);
  // REPLAY mode in a fresh session ignores whatever input is passed.
  auto s2 = newSession(CoreKind::Nestopia);
  REQUIRE(s2);
  for (auto& rec : log) REQUIRE(s2->step(rec.p1, rec.p2, rec.events).ok());
  REQUIRE(s2->seek(0).ok());
  s2->setMode(Mode::Replay);
  for (size_t i = 0; i < log.size(); ++i) {
    StepInfo si;
    REQUIRE(s2->step(0xFF, 0xFF, kEvPowerCycle, &si).ok());
    CHECK(si.applied == log[i]);
  }
  CHECK_EQ(s2->stateHash(), end);
  StepInfo si;
  REQUIRE(s2->step(0, 0, 0, &si).ok());
  CHECK(si.endOfTake);
  CHECK_EQ(si.frame, uint64_t(log.size()));
}

TEST_CASE("branching: rewind + re-record creates a branch, old take preserved, undo returns") {
  auto s = newSession(CoreKind::Nestopia);
  REQUIRE(s);
  auto a = DeterminismHarness::randomScript(600, 1);
  REQUIRE(recordScript(*s, a).ok());
  uint64_t takeA = s->activeTake();
  uint64_t endA = s->stateHash();
  REQUIRE(s->rewind(300).ok());
  CHECK_EQ(s->frame(), 300u);
  CHECK_EQ(s->takeLength(), 600u);  // rewinding alone never truncates
  auto b = DeterminismHarness::randomScript(150, 2);
  StepInfo si;
  REQUIRE(s->step(b[0].p1, b[0].p2, 0, &si).ok());
  CHECK(si.branched);
  for (size_t i = 1; i < b.size(); ++i) {
    REQUIRE(s->step(b[i].p1, b[i].p2, 0, &si).ok());
    CHECK_FALSE(si.branched);
  }
  uint64_t takeB = s->activeTake();
  CHECK(takeB != takeA);
  CHECK_EQ(s->takeLength(), 450u);
  CHECK_EQ(s->takes().size(), 2u);
  uint64_t endB = s->stateHash();
  // Old take intact.
  const Segment* segA = s->timeline().segment(takeA);
  REQUIRE(segA);
  CHECK_EQ(segA->records.size(), 600u);
  CHECK(segA->records == a);
  // New take = A[0..300) + B.
  std::vector<InputRecord> expectB(a.begin(), a.begin() + 300);
  expectB.insert(expectB.end(), b.begin(), b.end());
  CHECK(s->timeline().flattenActive() == expectB);
  CHECK_EQ(straightReplay(CoreKind::Nestopia, expectB).finalState, endB);
  // Undo ("前の試行へ戻す") goes back to take A at the branch frame.
  REQUIRE(s->undoTakeSwitch().ok());
  CHECK_EQ(s->activeTake(), takeA);
  CHECK_EQ(s->frame(), 300u);
  CHECK_EQ(s->takeLength(), 600u);
  REQUIRE(s->seek(600).ok());
  CHECK_EQ(s->stateHash(), endA);
  // Switch to B explicitly, then undo again.
  REQUIRE(s->activateTake(takeB).ok());
  REQUIRE(s->seek(450).ok());
  CHECK_EQ(s->stateHash(), endB);
  REQUIRE(s->undoTakeSwitch().ok());
  CHECK_EQ(s->activeTake(), takeA);
  CHECK_EQ(int(s->undoTakeSwitch().code), int(Err::NotFound));
  // Recording at the take end extends the leaf (no new branch).
  REQUIRE(s->seek(600).ok());
  REQUIRE(s->step(1, 2, 0, &si).ok());
  CHECK_FALSE(si.branched);
  CHECK_EQ(s->activeTake(), takeA);
  CHECK_EQ(s->takeLength(), 601u);
}

TEST_CASE("branch at frame 0 creates a second root take") {
  auto s = newSession(CoreKind::Mock);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(50, 1)).ok());
  REQUIRE(s->seek(0).ok());
  StepInfo si;
  REQUIRE(s->step(9, 9, 0, &si).ok());
  CHECK(si.branched);
  CHECK_EQ(s->timeline().segment(s->activeTake())->parent, 0u);
  CHECK_EQ(s->takeLength(), 1u);
}

TEST_CASE("seek correctness vs straight replay (nestopia, random targets)") {
  CheckpointPolicy pol;
  pol.denseInterval = 30;
  pol.sparseInterval = 600;
  auto s = newSession(CoreKind::Nestopia, pol);
  auto script = DeterminismHarness::randomScript(4000, 21, 1300, 4);
  REQUIRE(recordScript(*s, script).ok());
  Replay ref = straightReplay(CoreKind::Nestopia, script, true);
  std::mt19937 rng(5);
  for (int i = 0; i < 60; ++i) {
    uint64_t target = rng() % 4001;
    REQUIRE(s->seek(target).ok());
    CHECK_EQ(s->frame(), target);
    if (target > 0) {
      CHECK_EQ(s->stateHash(), ref.state[size_t(target - 1)]);
      CHECK_EQ(s->videoHash(), ref.video[size_t(target - 1)]);
    }
    size_t n;
    s->audio(&n);
    CHECK_EQ(n, 0u);  // seeking is silent
  }
  // seek(0) == power-on: following replay is bit-identical incl. audio.
  REQUIRE(s->seek(0).ok());
  s->setMode(Mode::Replay);
  bool same = true;
  for (size_t f = 0; f < 500; ++f) {
    REQUIRE(s->step(0, 0, 0).ok());
    same = same && s->videoHash() == ref.video[f] && s->audioHash() == ref.audio[f];
  }
  CHECK(same);
  CHECK_EQ(int(s->seek(4001).code), int(Err::OutOfRange));
}

static void stress(CoreKind kind, int iterations, uint64_t maxLen, size_t verifyTakes) {
  CheckpointPolicy pol;
  pol.denseInterval = 20;
  pol.denseCapacity = 200;
  pol.sparseInterval = 200;
  auto s = newSession(kind, pol);
  REQUIRE(s);
  std::mt19937_64 rng(1234);
  uint64_t branches = 0;
  for (int it = 0; it < iterations; ++it) {
    uint64_t op = rng() % 10;
    if (op < 4 && s->frame() > 0) {
      REQUIRE(s->rewind(1 + rng() % (s->frame() < 400 ? s->frame() : 400)).ok());
    } else if (op < 5 && s->takes().size() > 1) {
      auto t = s->takes();
      REQUIRE(s->activateTake(t[rng() % t.size()].id).ok());
    } else if (op < 6 && s->undoDepth() > 0) {
      REQUIRE(s->undoTakeSwitch().ok());
    } else {
      uint64_t n = 1 + rng() % 120;
      for (uint64_t k = 0; k < n && s->frame() < maxLen; ++k) {
        StepInfo si;
        uint64_t r = rng();
        REQUIRE(s->step(uint8_t(r), uint8_t(r >> 8), (r >> 16) % 97 == 0 ? kEvSoftReset : 0, &si).ok());
        branches += si.branched;
      }
    }
  }
  MESSAGE("iterations=" << iterations << " branches=" << branches << " takes=" << s->takes().size()
                        << " checkpoints=" << s->checkpoints().all().size());
  CHECK(branches > uint64_t(iterations / 10));
  // Every take replays identically through the session (checkpoints + seek) and from scratch.
  auto all = s->takes();
  std::shuffle(all.begin(), all.end(), rng);
  if (all.size() > verifyTakes) all.resize(verifyTakes);
  for (auto& t : all) {
    REQUIRE(s->activateTake(t.id).ok());
    REQUIRE(s->seek(s->takeLength()).ok());
    CHECK_EQ(s->stateHash(), straightReplay(kind, s->timeline().flattenActive(), false, false).finalState);
  }
  CHECK(s->checkpoints().all().size() < 200 + 1000);  // dense ring bounded
}

TEST_CASE("stress: 600 rewinds/branches/take switches (mock)") { stress(CoreKind::Mock, 600, 5000, 100000); }
TEST_CASE("stress: 300 rewinds/branches/take switches (nestopia)") { stress(CoreKind::Nestopia, 300, 2500, 10); }
