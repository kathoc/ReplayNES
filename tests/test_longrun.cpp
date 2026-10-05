// Long-running operation: 1 hour of frames (mock core) with periodic autosave/save/reopen,
// and ~8 minutes on the real core with frequent rewinds. Also reports memory/disk usage.
#include <chrono>
#include <random>

#include "persist/ProjectStore.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"

using namespace rn;
using namespace rntest;

static int64_t dirBytes(const std::string& dir) {
  int64_t total = 0;
  for (const char* sub : {"", "states", "timeline", "timeline/segments", "metadata", "journal"}) {
    std::vector<std::string> names;
    if (!fs::listDir(fs::join(dir, sub), names).ok()) continue;
    for (auto& n : names) total += fs::fileSize(fs::join(fs::join(dir, sub), n));
  }
  return total;
}

TEST_CASE("long run: 1 hour (216000 frames) recorded with autosave/save, reopened, verified (mock)") {
  std::string root = tempDir("longmock");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "hour.nesrec");
  std::vector<uint8_t> romBytes;
  fs::readFile(rom, romBytes);
  SessionOptions o;
  o.core = CoreKind::Mock;
  std::unique_ptr<Session> s;
  REQUIRE(Session::create(romBytes, rom, o, s).ok());
  REQUIRE(s->saveAs(dir).ok());
  std::mt19937_64 rng(99);
  const uint64_t hour = 216000;
  uint8_t p1 = 0, p2 = 0;
  int rewinds = 0;
  while (s->frame() < hour) {
    if (rng() % 8 == 0) { p1 = uint8_t(rng()); p2 = uint8_t(rng()); }
    REQUIRE(s->step(p1, p2, rng() % 20000 == 0 ? kEvSoftReset : 0).ok());
    uint64_t f = s->frame();
    if (f % 300 == 0) REQUIRE(s->autosave().ok());       // every 5 s
    if (f % 18000 == 0) REQUIRE(s->save().ok());         // every 5 min
    if (f % 1000 == 500 && rewinds < 200) {              // retakes
      REQUIRE(s->rewind(rng() % 400).ok());
      ++rewinds;
    }
  }
  REQUIRE(s->save().ok());
  uint64_t h = s->stateHash();
  auto log = s->timeline().flattenActive();
  MESSAGE("takes=" << s->takes().size() << " checkpoints=" << s->checkpoints().all().size()
                   << " cp-memory=" << s->checkpoints().memoryBytes() << "B disk=" << dirBytes(dir) << "B");
  CHECK(s->checkpoints().memoryBytes() < 4 * 1024 * 1024);
  CHECK(dirBytes(dir) < 32 * 1024 * 1024);
  s.reset();
  std::unique_ptr<Session> r;
  REQUIRE(ProjectStore::open(dir, "", SessionOptions(), r).ok());
  CHECK_EQ(r->frame(), log.size());
  CHECK_EQ(r->stateHash(), h);
  CHECK(r->timeline().flattenActive() == log);
  CHECK_EQ(straightReplay(CoreKind::Mock, log, false, false).finalState, h);
  REQUIRE(r->seek(1).ok());
  REQUIRE(r->seek(hour / 2).ok());
  REQUIRE(r->seek(r->takeLength()).ok());
  CHECK_EQ(r->stateHash(), h);
}

TEST_CASE("long run: 30000 frames on nestopia with hundreds of rewinds, save/reopen, verified") {
  std::string root = tempDir("longnst");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "long.nesrec");
  std::vector<uint8_t> romBytes;
  fs::readFile(rom, romBytes);
  SessionOptions o;
  std::unique_ptr<Session> s;
  REQUIRE(Session::create(romBytes, rom, o, s).ok());
  REQUIRE(s->saveAs(dir).ok());
  std::mt19937_64 rng(7);
  auto t0 = std::chrono::steady_clock::now();
  int rewinds = 0;
  uint8_t p1 = 0;
  while (s->frame() < 30000) {
    if (rng() % 6 == 0) p1 = uint8_t(rng());
    REQUIRE(s->step(p1, 0, 0).ok());
    if (s->frame() % 100 == 0 && rewinds < 250) {
      REQUIRE(s->rewind(1 + rng() % 90).ok());
      ++rewinds;
    }
    if (s->frame() % 600 == 0) REQUIRE(s->autosave().ok());
  }
  REQUIRE(s->save().ok());
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  uint64_t h = s->stateHash();
  auto log = s->timeline().flattenActive();
  MESSAGE("rewinds=" << rewinds << " takes=" << s->takes().size() << " time=" << secs << "s cp-memory="
                     << s->checkpoints().memoryBytes() << "B disk=" << dirBytes(dir) << "B");
  CHECK_EQ(rewinds, 250);
  s.reset();
  std::unique_ptr<Session> r;
  REQUIRE(ProjectStore::open(dir, "", SessionOptions(), r).ok());
  CHECK_EQ(r->stateHash(), h);
  CHECK_EQ(straightReplay(CoreKind::Nestopia, log, false, false).finalState, h);
}
