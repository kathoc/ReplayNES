// .nesrec persistence: save/close/reopen ("next day"), crash recovery via journal,
// corruption detection, core/ROM mismatch, ROM moved, disk full.
#include <cstdio>

#include "persist/ProjectStore.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"
#include "util/Json.h"

using namespace rn;
using namespace rntest;

namespace {
std::unique_ptr<Session> createProject(const std::string& dir, const std::string& romPath, CoreKind kind) {
  std::vector<uint8_t> rom;
  if (!fs::readFile(romPath, rom).ok()) return nullptr;
  SessionOptions o;
  o.core = kind;
  std::unique_ptr<Session> s;
  if (!Session::create(rom, romPath, o, s).ok()) return nullptr;
  if (!s->saveAs(dir).ok()) return nullptr;
  return s;
}

Status reopen(const std::string& dir, std::unique_ptr<Session>& out, const std::string& romOverride = "",
              OpenReport* rep = nullptr, bool dropCorrupt = false) {
  SessionOptions o;
  o.dropCorruptStates = dropCorrupt;
  return ProjectStore::open(dir, romOverride, o, out, rep);
}

void flipByte(const std::string& path, size_t offsetFromEnd) {
  std::vector<uint8_t> b;
  fs::readFile(path, b);
  b[b.size() - 1 - offsetFromEnd] ^= 0x5A;
  fs::writeFileAtomic(path, b.data(), b.size());
}

void editManifest(const std::string& dir, const std::string& key, Json value) {
  std::string t;
  fs::readText(fs::join(dir, "manifest.json"), t);
  Json j;
  Json::parse(t, j);
  j.set(key, value);
  fs::writeFileAtomic(fs::join(dir, "manifest.json"), j.dump());
}
}  // namespace

TEST_CASE("persist: save, close, reopen next day, head restored, rewind further back (nestopia)") {
  std::string root = tempDir("nextday");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "Run.nesrec");
  auto s = createProject(dir, rom, CoreKind::Nestopia);
  REQUIRE(s);
  auto a = DeterminismHarness::randomScript(2400, 31, 800, 5);
  REQUIRE(recordScript(*s, std::vector<InputRecord>(a.begin(), a.begin() + 1200)).ok());
  uint64_t bm1;
  REQUIRE(s->bookmarkAdd("ステージ1", &bm1).ok());
  REQUIRE(recordScript(*s, std::vector<InputRecord>(a.begin() + 1200, a.end())).ok());
  REQUIRE(s->rewind(500).ok());  // mistake -> rewind and retake
  auto b = DeterminismHarness::randomScript(700, 32);
  REQUIRE(recordScript(*s, b).ok());
  uint64_t head = s->activeTake(), frame = s->frame(), len = s->takeLength(), hash = s->stateHash();
  size_t takes = s->takes().size();
  REQUIRE(s->save().ok());
  CHECK_FALSE(s->unsavedChanges());
  s.reset();  // app quit

  std::unique_ptr<Session> r;
  OpenReport rep;
  REQUIRE(reopen(dir, r, "", &rep).ok());
  CHECK_FALSE(rep.journalApplied);
  CHECK_EQ(r->activeTake(), head);
  CHECK_EQ(r->frame(), frame);
  CHECK_EQ(r->takeLength(), len);
  CHECK_EQ(r->takes().size(), takes);
  CHECK_EQ(r->stateHash(), hash);
  REQUIRE_EQ(r->bookmarks().size(), size_t(1));
  CHECK_EQ(r->bookmarks()[0].name, std::string("ステージ1"));
  CHECK_EQ(r->undoDepth(), size_t(1));
  // Rewind further back than anything in memory before the restart.
  std::vector<InputRecord> active = r->timeline().flattenActive();
  Replay ref = straightReplay(CoreKind::Nestopia, active, true);
  for (uint64_t t : {uint64_t(1), uint64_t(77), uint64_t(601), uint64_t(1199), uint64_t(2000), len}) {
    REQUIRE(r->seek(t).ok());
    if (r->stateHash() != ref.state[size_t(t - 1)]) MESSAGE("seek mismatch at t=" << t);
    CHECK_EQ(r->stateHash(), ref.state[size_t(t - 1)]);
  }
  REQUIRE(r->bookmarkGoto(r->bookmarks()[0].id).ok());
  CHECK_EQ(r->frame(), 1200u);
  CHECK_EQ(r->stateHash(), ref.state[1199]);
  // Keep recording on day 2 (branch from the bookmark), save, reopen again.
  REQUIRE(recordScript(*r, DeterminismHarness::randomScript(100, 33)).ok());
  uint64_t h2 = r->stateHash();
  REQUIRE(r->save().ok());
  r.reset();
  REQUIRE(reopen(dir, r).ok());
  CHECK_EQ(r->frame(), 1300u);
  CHECK_EQ(r->stateHash(), h2);
  REQUIRE(r->undoTakeSwitch().ok());
  CHECK_EQ(r->activeTake(), head);
}

TEST_CASE("persist: crash recovery from journal (kill before full save)") {
  std::string root = tempDir("crash");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "c.nesrec");
  auto s = createProject(dir, rom, CoreKind::Mock);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(1000, 1)).ok());
  REQUIRE(s->save().ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(500, 2)).ok());
  REQUIRE(s->rewind(200).ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(300, 3)).ok());  // branch after save
  uint64_t bm;
  REQUIRE(s->bookmarkAdd("after save", &bm).ok());
  REQUIRE(s->autosave().ok());
  auto expected = s->timeline().flattenActive();
  uint64_t frame = s->frame(), head = s->activeTake();
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(50, 4)).ok());  // lost: never autosaved
  s.reset();  // simulated kill: no save

  std::unique_ptr<Session> r;
  OpenReport rep;
  REQUIRE(reopen(dir, r, "", &rep).ok());
  CHECK(rep.journalApplied);
  CHECK(r->recovered());
  CHECK(r->unsavedChanges());
  CHECK_EQ(r->activeTake(), head);
  CHECK_EQ(r->frame(), frame);
  CHECK(r->timeline().flattenActive() == expected);
  CHECK_EQ(r->takes().size(), 2u);
  REQUIRE_EQ(r->bookmarks().size(), size_t(1));
  CHECK_EQ(r->stateHash(), straightReplay(CoreKind::Mock, expected).finalState);
  // After a full save the journal is folded in.
  REQUIRE(r->save().ok());
  r.reset();
  REQUIRE(reopen(dir, r, "", &rep).ok());
  CHECK_FALSE(rep.journalApplied);
  CHECK(r->timeline().flattenActive() == expected);
}

TEST_CASE("persist: torn journal tail is discarded, mid-journal corruption is an error") {
  std::string root = tempDir("torn");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "t.nesrec");
  auto s = createProject(dir, rom, CoreKind::Mock);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(300, 1)).ok());
  REQUIRE(s->autosave().ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(300, 2)).ok());
  REQUIRE(s->autosave().ok());
  auto expected = s->timeline().flattenActive();
  s.reset();
  std::string jp = fs::join(dir, "journal/journal.bin");
  std::vector<uint8_t> j;
  fs::readFile(jp, j);
  // Torn write: a partial record at the end.
  std::vector<uint8_t> torn = j;
  torn.insert(torn.end(), {0x40, 0x00, 0x00, 0x00, 0x02, 0x01});
  fs::writeFileAtomic(jp, torn.data(), torn.size());
  std::unique_ptr<Session> r;
  OpenReport rep;
  REQUIRE(reopen(dir, r, "", &rep).ok());
  CHECK(rep.journalTornTail);
  CHECK(r->timeline().flattenActive() == expected);
  CHECK_EQ(fs::fileSize(jp), int64_t(j.size()));  // truncated back to the last good record
  r.reset();
  // Corrupt a byte inside the FIRST record (not the tail) -> explicit corruption error.
  const size_t firstRecord = 20;  // journal header size
  for (size_t off : {firstRecord + 8 /* type byte */, firstRecord + 4 + 3 /* length high byte */}) {
    std::vector<uint8_t> bad = j;
    bad[off] ^= 0xFF;
    fs::writeFileAtomic(jp, bad.data(), bad.size());
    Status st = reopen(dir, r);
    CHECK_EQ(int(st.code), int(Err::Corrupt));
  }
}

TEST_CASE("persist: crash between index and manifest commit opens the newer index") {
  std::string root = tempDir("commit");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "m.nesrec");
  auto s = createProject(dir, rom, CoreKind::Mock);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(400, 1)).ok());
  REQUIRE(s->save().ok());
  std::string oldManifest;
  fs::readText(fs::join(dir, "manifest.json"), oldManifest);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(400, 2)).ok());
  REQUIRE(s->save().ok());
  auto expected = s->timeline().flattenActive();
  s.reset();
  fs::writeFileAtomic(fs::join(dir, "manifest.json"), oldManifest);  // manifest write "lost"
  std::unique_ptr<Session> r;
  REQUIRE(reopen(dir, r).ok());
  CHECK(r->timeline().flattenActive() == expected);
}

TEST_CASE("persist: corrupt segment / state are detected; states can be dropped explicitly") {
  std::string root = tempDir("corrupt");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "x.nesrec");
  auto s = createProject(dir, rom, CoreKind::Nestopia);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(1300, 1)).ok());
  REQUIRE(s->save().ok());
  uint64_t hash = s->stateHash();
  s.reset();
  std::string seg = fs::join(dir, "timeline/segments/1.seg");
  std::vector<uint8_t> segBytes;
  fs::readFile(seg, segBytes);
  flipByte(seg, 10);
  std::unique_ptr<Session> r;
  Status st = reopen(dir, r);
  CHECK_EQ(int(st.code), int(Err::Corrupt));
  CHECK(st.message.find("1.seg") != std::string::npos);
  fs::writeFileAtomic(seg, segBytes.data(), segBytes.size());

  std::vector<std::string> states;
  fs::listDir(fs::join(dir, "states"), states);
  REQUIRE(!states.empty());
  std::string sp = fs::join(dir, "states/" + states[0]);
  flipByte(sp, 100);
  st = reopen(dir, r);
  CHECK_EQ(int(st.code), int(Err::Corrupt));
  CHECK(st.message.find("drop-corrupt-states") != std::string::npos);
  OpenReport rep;
  REQUIRE(reopen(dir, r, "", &rep, true).ok());  // explicit opt-in, reported
  CHECK_EQ(rep.droppedStates.size(), size_t(1));
  CHECK_EQ(r->stateHash(), hash);  // correctness unaffected: states are only a cache
  // A truncated segment file is corruption too.
  r.reset();
  fs::writeFileAtomic(seg, segBytes.data(), segBytes.size() / 2);
  CHECK_EQ(int(reopen(dir, r, "", nullptr, true).code), int(Err::Corrupt));
}

TEST_CASE("persist: core compat mismatch is refused with a distinct error") {
  std::string root = tempDir("compat");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "k.nesrec");
  auto s = createProject(dir, rom, CoreKind::Nestopia);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(100, 1)).ok());
  REQUIRE(s->save().ok());
  s.reset();
  editManifest(dir, "coreCompatId", "nestopia-ue@000000000000+p1+adapter1+ntsc");
  std::unique_ptr<Session> r;
  Status st = reopen(dir, r);
  CHECK_EQ(int(st.code), int(Err::CoreMismatch));
  CHECK(st.message.find("000000000000") != std::string::npos);
  editManifest(dir, "coreCompatId", coreCompatId(CoreKind::Nestopia));
  REQUIRE(reopen(dir, r).ok());
  r.reset();
  editManifest(dir, "formatVersion", int64_t(kProjectFormatVersion + 1));
  CHECK_EQ(int(reopen(dir, r).code), int(Err::UnsupportedFormat));
}

TEST_CASE("persist: ROM SHA mismatch refused; moved ROM reopened with override path") {
  std::string root = tempDir("rom");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "r.nesrec");
  auto s = createProject(dir, rom, CoreKind::Mock);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(100, 1)).ok());
  REQUIRE(s->save().ok());
  uint64_t hash = s->stateHash();
  s.reset();
  // A different ROM at an override path.
  std::vector<uint8_t> other = buildTestRom();
  other[16 + 100] ^= 1;
  std::string otherPath = fs::join(root, "other.nes");
  fs::writeFileAtomic(otherPath, other.data(), other.size());
  std::unique_ptr<Session> r;
  CHECK_EQ(int(reopen(dir, r, otherPath).code), int(Err::RomMismatch));
  // Moved ROM: stored path missing.
  std::string moved = fs::join(root, "moved/somewhere/game.nes");
  fs::createDirs(fs::join(root, "moved/somewhere"));
  std::rename(rom.c_str(), moved.c_str());
  Status st = reopen(dir, r);
  CHECK_EQ(int(st.code), int(Err::RomNotFound));
  REQUIRE(reopen(dir, r, moved).ok());
  CHECK_EQ(r->stateHash(), hash);
  CHECK_EQ(r->romPath(), moved);
  REQUIRE(r->save().ok());  // new location remembered
  r.reset();
  REQUIRE(reopen(dir, r).ok());
  CHECK_EQ(r->romPath(), moved);
  // The ROM itself is never stored in the package.
  std::vector<std::string> names;
  for (const char* sub : {"", "states", "timeline", "timeline/segments", "metadata", "journal"}) {
    fs::listDir(fs::join(dir, sub), names);
    for (auto& n : names) CHECK(n.find(".nes") == std::string::npos || n.find(".nesrec") != std::string::npos);
  }
}

TEST_CASE("persist: disk full during save/autosave is reported and leaves a consistent project") {
  std::string root = tempDir("diskfull");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "d.nesrec");
  auto s = createProject(dir, rom, CoreKind::Mock);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(700, 1)).ok());
  REQUIRE(s->save().ok());
  auto committed = s->timeline().flattenActive();
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(700, 2)).ok());
  fs::setDiskFullAfterBytes(64);
  Status st = s->save();
  fs::setDiskFullAfterBytes(-1);
  CHECK_EQ(int(st.code), int(Err::DiskFull));
  {
    std::unique_ptr<Session> r;
    REQUIRE(reopen(dir, r).ok());
    CHECK(r->timeline().flattenActive() == committed);
  }
  // Autosave failure is rolled back; the next autosave succeeds and is recoverable.
  int64_t before = fs::fileSize(fs::join(dir, "journal/journal.bin"));
  fs::setDiskFullAfterBytes(10);
  st = s->autosave();
  fs::setDiskFullAfterBytes(-1);
  CHECK_EQ(int(st.code), int(Err::DiskFull));
  CHECK_EQ(fs::fileSize(fs::join(dir, "journal/journal.bin")), before);
  REQUIRE(s->autosave().ok());
  auto all = s->timeline().flattenActive();
  s.reset();
  std::unique_ptr<Session> r;
  REQUIRE(reopen(dir, r).ok());
  CHECK(r->recovered());
  CHECK(r->timeline().flattenActive() == all);
}

TEST_CASE("persist: refuses to create into a non-empty directory; in-memory save needs a dir") {
  std::string root = tempDir("nonempty");
  std::string rom = writeTestRom(root);
  auto s = newSession(CoreKind::Mock);
  CHECK_EQ(int(s->save().code), int(Err::InvalidArg));
  CHECK_EQ(int(s->saveAs(root).code), int(Err::AlreadyExists));
  REQUIRE(s->saveAs(fs::join(root, "ok.nesrec")).ok());
  CHECK_EQ(int(s->saveAs(fs::join(root, "again.nesrec")).code), int(Err::AlreadyExists));
}
