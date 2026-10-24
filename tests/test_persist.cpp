// .nesrec persistence: save/close/reopen ("next day"), crash recovery via journal,
// corruption detection, core/ROM mismatch, ROM moved, disk full.
#include <cstdio>

#include "core/MockCore.h"
#include "persist/ProjectStore.h"
#include "util/Bytes.h"
#include "util/Hash.h"
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
// Count actual core calls independently of the restored logical frame counter.
class CountingPersistCore final : public ICore {
 public:
  explicit CountingPersistCore(uint64_t& calls) : calls_(calls) {}
  Status loadROM(const uint8_t* p, size_t n) override { return mock_.loadROM(p, n); }
  Status powerCycle() override { return mock_.powerCycle(); }
  Status softReset() override { return mock_.softReset(); }
  Status stepFrame(uint8_t a, uint8_t b, bool render) override {
    ++calls_;
    return mock_.stepFrame(a, b, render);
  }
  uint64_t frameIndex() const override { return mock_.frameIndex(); }
  const uint32_t* video() const override { return mock_.video(); }
  const int16_t* audio(size_t* n) const override { return mock_.audio(n); }
  Status saveState(std::vector<uint8_t>& b) override { return mock_.saveState(b); }
  Status loadState(const uint8_t* p, size_t n) override { return mock_.loadState(p, n); }
  std::string compatId() const override { return "counted-persist/1"; }
  std::string buildId() const override { return "test"; }
  uint32_t stateFormatVersion() const override { return 1; }
  CoreKind kind() const override { return CoreKind::External; }
 private:
  MockCore mock_;
  uint64_t& calls_;
};

struct PersistCoreRegistration {
  explicit PersistCoreRegistration(uint64_t& calls) {
    registerExternalCore({"counted-persist", "counted-persist/1",
                          uint32_t(CapAudio | CapState | CapSoftReset | CapRecording),
                          [&calls] { return std::unique_ptr<ICore>(new CountingPersistCore(calls)); }});
  }
  ~PersistCoreRegistration() { unregisterExternalCore(); }
};

struct JournalRecordRange { size_t start, end; uint8_t type; };
std::vector<JournalRecordRange> journalRanges(const std::vector<uint8_t>& bytes) {
  std::vector<JournalRecordRange> out;
  ByteReader r(bytes.data(), bytes.size());
  r.ptr(20);
  while (r.remaining()) {
    size_t start = r.pos();
    r.u32();
    uint32_t len = r.u32();
    const uint8_t* body = r.ptr(len);
    r.u32();
    if (!r.ok() || !body || !len) return {};
    out.push_back({start, r.pos(), body[0]});
  }
  return out;
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

TEST_CASE("persist: autosave sparse states restore without replaying from zero and fold into full save") {
  uint64_t calls = 0;
  PersistCoreRegistration registration(calls);
  const std::string root = tempDir("autosave-sparse");
  const std::string dir = fs::join(root, "s.nesrec");
  auto s = createProject(dir, writeTestRom(root), CoreKind::External);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(1900, 71)).ok());
  const uint64_t hash = s->stateHash();
  std::vector<uint64_t> sparseIds;
  for (const auto& kv : s->checkpoints().all())
    if (kv.second.kind == CpKind::Sparse) sparseIds.push_back(kv.first);
  REQUIRE_EQ(sparseIds.size(), size_t(3));
  REQUIRE(s->autosave().ok());
  const std::string jp = fs::join(dir, "journal/journal.bin");
  const int64_t journalSize = fs::fileSize(jp);
  REQUIRE(s->autosave().ok());
  CHECK_EQ(fs::fileSize(jp), journalSize);  // each sparse state is journaled once
  for (uint64_t id : sparseIds) {
    CHECK(s->checkpoints().get(id)->persisted);
    CHECK(fs::exists(fs::join(dir, "states/" + std::to_string(id) + ".state")));
  }
  s.reset();
  calls = 0;
  std::unique_ptr<Session> r;
  REQUIRE(reopen(dir, r).ok());
  CHECK_EQ(calls, uint64_t(100));  // restoring the cursor starts at sparse frame 1800
  CHECK_EQ(r->stateHash(), hash);
  for (uint64_t id : sparseIds) {
    REQUIRE(r->checkpoints().get(id));
    CHECK(r->checkpoints().get(id)->persisted);
  }
  REQUIRE(r->seek(0).ok());
  calls = 0;
  REQUIRE(r->seek(1900).ok());
  CHECK(calls <= 100);
  CHECK(calls > 0);
  CHECK_EQ(r->stateHash(), hash);
  REQUIRE(fs::writeFileAtomic(fs::join(dir, "states/orphan.state"), "orphan").ok());
  REQUIRE(r->save().ok());
  CHECK_EQ(fs::fileSize(jp), int64_t(20));
  CHECK_FALSE(fs::exists(fs::join(dir, "states/orphan.state")));
  std::string index;
  REQUIRE(fs::readText(fs::join(dir, "timeline/index.json"), index).ok());
  Json idx;
  REQUIRE(Json::parse(index, idx));
  for (uint64_t id : sparseIds) {
    bool indexed = false;
    for (const auto& e : idx["checkpoints"].items())
      if (uint64_t(e["id"].asInt()) == id) indexed = true;
    CHECK(indexed);
    CHECK(fs::exists(fs::join(dir, "states/" + std::to_string(id) + ".state")));
  }
  r.reset();
  OpenReport report;
  REQUIRE(reopen(dir, r, "", &report).ok());
  CHECK_FALSE(report.journalApplied);
  for (uint64_t id : sparseIds) REQUIRE(r->checkpoints().get(id));
  CHECK_EQ(r->stateHash(), hash);
}

TEST_CASE("persist: unreferenced sparse file, torn sparse record and old journal recover safely") {
  const std::string root = tempDir("sparse-crash");
  const std::string dir = fs::join(root, "s.nesrec");
  auto s = createProject(dir, writeTestRom(root), CoreKind::Mock);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(1300, 72)).ok());
  const uint64_t hash = s->stateHash();
  REQUIRE(s->seek(0).ok());  // recovery must not generate checkpoints while restoring the cursor
  REQUIRE(s->autosave().ok());
  s.reset();
  const std::string jp = fs::join(dir, "journal/journal.bin");
  std::vector<uint8_t> bytes;
  REQUIRE(fs::readFile(jp, bytes).ok());
  auto ranges = journalRanges(bytes);
  REQUIRE(!ranges.empty());
  size_t sparseStart = 0, sparseEnd = 0;
  std::vector<uint8_t> legacy(bytes.begin(), bytes.begin() + 20);
  for (const auto& rec : ranges) {
    if (rec.type == 5) {
      if (!sparseStart) { sparseStart = rec.start; sparseEnd = rec.end; }
    } else {
      legacy.insert(legacy.end(), bytes.begin() + rec.start, bytes.begin() + rec.end);
    }
  }
  REQUIRE(sparseStart > 20);
  // The state file is durable, but its record has not reached disk (or is torn).
  for (size_t cut : {sparseStart, sparseStart + 9, sparseEnd - 1}) {
    REQUIRE(fs::writeFileAtomic(jp, bytes.data(), cut).ok());
    std::unique_ptr<Session> r;
    OpenReport report;
    REQUIRE(reopen(dir, r, "", &report).ok());
    CHECK_EQ(report.journalTornTail, cut != sparseStart);
    CHECK_EQ(fs::fileSize(jp), int64_t(sparseStart));
    CHECK_EQ(r->checkpoints().all().size(), size_t(0));
    CHECK_EQ(r->takeLength(), uint64_t(1300));
    REQUIRE(r->seek(1300).ok());
    CHECK_EQ(r->stateHash(), hash);
  }
  // A journal containing only the pre-extension record types opens unchanged.
  REQUIRE(fs::writeFileAtomic(jp, legacy.data(), legacy.size()).ok());
  std::unique_ptr<Session> r;
  OpenReport report;
  REQUIRE(reopen(dir, r, "", &report).ok());
  CHECK(report.journalApplied);
  CHECK_FALSE(report.journalTornTail);
  CHECK_EQ(r->frame(), uint64_t(0));
  CHECK_EQ(r->checkpoints().all().size(), size_t(0));
  REQUIRE(r->seek(1300).ok());
  CHECK_EQ(r->stateHash(), hash);
}

TEST_CASE("persist: journaled sparse state rejects a different core compat id") {
  const std::string root = tempDir("sparse-compat");
  const std::string dir = fs::join(root, "s.nesrec");
  auto s = createProject(dir, writeTestRom(root), CoreKind::Mock);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(650, 73)).ok());
  REQUIRE(s->autosave().ok());
  uint64_t id = 0;
  for (const auto& kv : s->checkpoints().all())
    if (kv.second.kind == CpKind::Sparse) id = kv.first;
  REQUIRE(id != 0);
  s.reset();
  const std::string sp = fs::join(dir, "states/" + std::to_string(id) + ".state");
  std::vector<uint8_t> bytes;
  REQUIRE(fs::readFile(sp, bytes).ok());
  REQUIRE(fs::removeFile(sp).ok());
  std::unique_ptr<Session> dropped;
  CHECK_EQ(int(reopen(dir, dropped).code), int(Err::Corrupt));
  OpenReport report;
  REQUIRE(reopen(dir, dropped, "", &report, true).ok());
  CHECK_EQ(report.droppedStates.size(), size_t(1));
  CHECK(dropped->checkpoints().nextId() > id);
  CHECK_FALSE(dropped->checkpoints().get(id));
  REQUIRE(dropped->autosave().ok());  // regenerated cache gets a new durable id
  dropped.reset();
  REQUIRE(reopen(dir, dropped, "", &report, true).ok());
  CHECK_EQ(dropped->frame(), uint64_t(650));
  dropped.reset();
  REQUIRE(fs::writeFileAtomic(sp, bytes.data(), bytes.size()).ok());
  // Keep state bytes and their journal CRC intact, but change the file's core identity.
  ByteReader header(bytes.data(), bytes.size());
  header.ptr(37);  // magic/version, id/frame/owner, kind, state format
  const uint64_t compatLength = header.varint();
  REQUIRE(compatLength > 0);
  bytes[header.pos()] ^= 1;
  const uint32_t crc = crc32(bytes.data(), bytes.size() - 4);
  for (int i = 0; i < 4; ++i) bytes[bytes.size() - 4 + size_t(i)] = uint8_t(crc >> (8 * i));
  REQUIRE(fs::writeFileAtomic(sp, bytes.data(), bytes.size()).ok());
  std::unique_ptr<Session> r;
  CHECK_EQ(int(reopen(dir, r).code), int(Err::CoreMismatch));
  CHECK_EQ(int(reopen(dir, r, "", nullptr, true).code), int(Err::CoreMismatch));
}

TEST_CASE("persist: failed journal append leaves sparse state retryable without duplicate records") {
  const std::string root = tempDir("sparse-retry");
  const std::string dir = fs::join(root, "s.nesrec");
  auto s = createProject(dir, writeTestRom(root), CoreKind::Mock);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(650, 74)).ok());
  const Checkpoint* sparse = nullptr;
  for (const auto& kv : s->checkpoints().all())
    if (kv.second.kind == CpKind::Sparse) sparse = &kv.second;
  REQUIRE(sparse);
  REQUIRE(sparse->compat.size() < 128);  // one-byte LEB128 string length in this fixture
  // FILE_FORMAT.md: 50 fixed bytes (including the string length), compat, state payload.
  const size_t stateSize = 50 + sparse->compat.size() + sparse->data.size();
  const std::string sp = fs::join(dir, "states/" + std::to_string(sparse->id) + ".state");
  const std::string jp = fs::join(dir, "journal/journal.bin");
  const int64_t before = fs::fileSize(jp);
  fs::setDiskFullAfterBytes(int64_t(stateSize + 10));
  Status st = s->autosave();
  fs::setDiskFullAfterBytes(-1);
  CHECK_EQ(int(st.code), int(Err::DiskFull));
  CHECK_EQ(fs::fileSize(sp), int64_t(stateSize));  // state completed before journal failed
  CHECK_EQ(fs::fileSize(jp), before);
  CHECK_FALSE(sparse->persisted);
  REQUIRE(s->autosave().ok());
  CHECK(sparse->persisted);
  std::vector<uint8_t> bytes;
  REQUIRE(fs::readFile(jp, bytes).ok());
  size_t references = 0;
  for (const auto& rec : journalRanges(bytes)) if (rec.type == 5) ++references;
  CHECK_EQ(references, size_t(1));
  REQUIRE(s->autosave().ok());
  CHECK_EQ(fs::fileSize(jp), int64_t(bytes.size()));
  const uint64_t hash = s->stateHash();
  s.reset();
  std::unique_ptr<Session> r;
  REQUIRE(reopen(dir, r).ok());
  CHECK_EQ(r->frame(), uint64_t(650));
  CHECK_EQ(r->stateHash(), hash);
}
