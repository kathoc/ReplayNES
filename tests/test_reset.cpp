// "Reset Project" (Session::reset / rn_session_reset): the project starts over from power-on with
// an empty timeline; A/B slots optionally kept; persistence (full save, journal reset, old files
// collected, reopen) and crash safety (a failure at any write leaves the old or the reset project).
#include <algorithm>

#include "harness/DeterminismHarness.h"
#include "persist/ProjectStore.h"
#include "replaynes/replaynes.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"

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

Status reopen(const std::string& dir, std::unique_ptr<Session>& out, OpenReport* rep = nullptr) {
  SessionOptions o;
  return ProjectStore::open(dir, "", o, out, rep);
}

// Two takes (a branch), the undo history, a bookmark and A/B slots 0 (A+B) and 2 (A only).
Status fillProject(Session& s) {
  RN_TRY(recordScript(s, DeterminismHarness::randomScript(700, 11)));
  RN_TRY(s.practiceSetA(0));
  RN_TRY(recordScript(s, DeterminismHarness::randomScript(90, 12)));
  RN_TRY(s.practiceSetB(0));
  uint64_t bm = 0;
  RN_TRY(s.bookmarkAdd("before branch", &bm));
  RN_TRY(s.rewind(300));
  RN_TRY(recordScript(s, DeterminismHarness::randomScript(250, 13)));  // branch: a second take
  RN_TRY(s.practiceSetA(2));
  return Status::Ok();
}

void checkEmpty(const Session& s) {
  CHECK_EQ(s.frame(), 0u);
  CHECK_EQ(s.takeLength(), 0u);
  CHECK(s.takes().empty());
  CHECK_EQ(s.activeTake(), 0u);
  CHECK(s.bookmarks().empty());
  CHECK_EQ(s.undoDepth(), size_t(0));
  CHECK(s.mode() == Mode::Record);
}

size_t files(const std::string& dir) {
  std::vector<std::string> names;
  fs::listDir(dir, names);
  return names.size();
}
}  // namespace

TEST_CASE("reset: empty timeline, no takes/bookmarks/undo, state == fresh power-on (nestopia)") {
  auto fresh = newSession(CoreKind::Nestopia);
  auto s = newSession(CoreKind::Nestopia);
  REQUIRE(fresh);
  REQUIRE(s);
  const uint64_t power = fresh->stateHash();
  REQUIRE(fillProject(*s).ok());
  REQUIRE(s->takes().size() >= 2u);
  REQUIRE(s->setMode(Mode::Replay).ok());
  REQUIRE(s->reset(false).ok());
  checkEmpty(*s);
  CHECK_EQ(s->stateHash(), power);
  for (int i = 0; i < kPracticeSlots; ++i) CHECK_FALSE(s->practiceSlot(i)->used);
  CHECK(s->checkpoints().all().empty());
  size_t n = 1;
  s->audio(&n);
  CHECK_EQ(n, size_t(0));
  // Recording afterwards is exactly a new project's recording.
  auto script = DeterminismHarness::randomScript(400, 21);
  REQUIRE(recordScript(*s, script).ok());
  CHECK(s->timeline().flattenActive() == script);
  CHECK_EQ(s->takes().size(), size_t(1));
  CHECK_EQ(s->stateHash(), straightReplay(CoreKind::Nestopia, script).finalState);
  REQUIRE(s->seek(123).ok());  // checkpoints / seeking work on the new timeline
  REQUIRE(recordScript(*fresh, script).ok());
  REQUIRE(fresh->seek(123).ok());
  CHECK_EQ(s->stateHash(), fresh->stateHash());
}

TEST_CASE("reset: A/B slots kept on request (A still loads), take position cleared") {
  auto s = newSession(CoreKind::Nestopia);
  REQUIRE(s);
  REQUIRE(fillProject(*s).ok());
  const PracticeSlot before0 = *s->practiceSlot(0);
  REQUIRE(before0.used);
  REQUIRE(before0.hasB);
  REQUIRE(s->practiceGotoA(0).ok());
  const uint64_t hashA = s->stateHash();
  CHECK_EQ(int(s->reset(true).code), int(Err::WrongMode));  // leave practice first
  REQUIRE(s->setMode(Mode::Record).ok());
  CHECK_EQ(s->takeLength(), 740u);  // refused reset changed nothing
  REQUIRE(s->reset(true).ok());
  checkEmpty(*s);
  const PracticeSlot* p0 = s->practiceSlot(0);
  CHECK(p0->used);
  CHECK(p0->hasB);
  CHECK_EQ(p0->length, before0.length);
  CHECK(p0->state == before0.state);
  CHECK_FALSE(p0->hasTakeFrame);
  CHECK_EQ(p0->takeId, 0u);
  CHECK(s->practiceSlot(2)->used);
  CHECK_FALSE(s->practiceSlot(1)->used);
  CHECK_FALSE(s->practiceBSettable(0));  // continuity since A is gone
  REQUIRE(s->practiceGotoA(0).ok());
  CHECK_EQ(s->stateHash(), hashA);
  REQUIRE(s->setMode(Mode::Record).ok());
  CHECK_EQ(s->frame(), 0u);
  CHECK_EQ(s->takeLength(), 0u);
}

TEST_CASE("reset: saved project is fully saved, old files collected, reopens reset; recording continues") {
  std::string root = tempDir("reset-save");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "r.nesrec");
  auto s = createProject(dir, rom, CoreKind::Mock);
  REQUIRE(s);
  REQUIRE(fillProject(*s).ok());
  REQUIRE(s->save().ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(100, 14)).ok());
  REQUIRE(s->autosave().ok());  // journal has content of the old generation
  REQUIRE(files(fs::join(dir, "timeline/segments")) >= 2u);
  REQUIRE(s->reset(true).ok());
  CHECK_FALSE(s->unsavedChanges());
  CHECK_EQ(files(fs::join(dir, "timeline/segments")), size_t(0));
  // Only the kept A/B states remain.
  std::vector<std::string> states;
  fs::listDir(fs::join(dir, "states"), states);
  CHECK_EQ(states.size(), size_t(2));
  for (auto& n : states) CHECK(n.rfind("practice-", 0) == 0);
  CHECK_EQ(fs::fileSize(fs::join(dir, "journal/journal.bin")), int64_t(20));  // header only
  {
    std::unique_ptr<Session> r;
    OpenReport rep;
    REQUIRE(reopen(dir, r, &rep).ok());
    CHECK_FALSE(rep.journalApplied);
    checkEmpty(*r);
    CHECK(r->practiceSlot(0)->used);
    CHECK(r->practiceSlot(2)->used);
    CHECK_FALSE(r->practiceSlot(0)->hasTakeFrame);
  }
  // Record on the reset project: autosave (crash) and full save both reopen correctly; ids are
  // not reused (new segment / checkpoint files never collide with the old generation's).
  auto script = DeterminismHarness::randomScript(900, 15);
  REQUIRE(recordScript(*s, script).ok());
  CHECK(s->activeTake() > 2u);
  REQUIRE(s->autosave().ok());
  s.reset();
  std::unique_ptr<Session> r;
  OpenReport rep;
  REQUIRE(reopen(dir, r, &rep).ok());
  CHECK(rep.journalApplied);
  CHECK(r->timeline().flattenActive() == script);
  CHECK_EQ(r->stateHash(), straightReplay(CoreKind::Mock, script).finalState);
  REQUIRE(r->save().ok());
  r.reset();
  REQUIRE(reopen(dir, r).ok());
  CHECK(r->timeline().flattenActive() == script);
  CHECK_EQ(r->takes().size(), size_t(1));
  // Reset without keeping the slots: their files are collected too.
  REQUIRE(r->reset(false).ok());
  states.clear();
  fs::listDir(fs::join(dir, "states"), states);
  CHECK(states.empty());
  r.reset();
  REQUIRE(reopen(dir, r).ok());
  checkEmpty(*r);
  for (int i = 0; i < kPracticeSlots; ++i) CHECK_FALSE(r->practiceSlot(i)->used);
}

TEST_CASE("reset: a crash/failure at any write leaves either the old or the reset project") {
  std::string root = tempDir("reset-crash");
  std::string rom = writeTestRom(root);
  int sawOld = 0, sawNew = 0, rolledBack = 0;
  bool done = false;
  for (int64_t budget = 0; !done && budget < (int64_t(1) << 22); budget += budget < 64 ? 8 : 61) {
    std::string dir = fs::join(root, "c" + std::to_string(budget) + ".nesrec");
    auto s = createProject(dir, rom, CoreKind::Mock);
    REQUIRE(s);
    REQUIRE(fillProject(*s).ok());
    REQUIRE(s->save().ok());
    REQUIRE(recordScript(*s, DeterminismHarness::randomScript(120, 16)).ok());
    uint64_t bm = 0;
    REQUIRE(s->bookmarkAdd("journaled", &bm).ok());
    REQUIRE(s->autosave().ok());
    const auto oldLog = s->timeline().flattenActive();
    const size_t oldTakes = s->takes().size();
    const uint64_t oldFrame = s->frame(), oldHash = s->stateHash();

    fs::setDiskFullAfterBytes(budget);
    Status st = s->reset(true);
    fs::setDiskFullAfterBytes(-1);
    if (st.ok()) {
      done = true;
    } else {
      CHECK_EQ(int(st.code), int(Err::DiskFull));
      if (s->takeLength() != 0) {
        // Not committed: the session is exactly as before.
        ++rolledBack;
        CHECK(s->timeline().flattenActive() == oldLog);
        CHECK_EQ(s->takes().size(), oldTakes);
        CHECK_EQ(s->frame(), oldFrame);
        CHECK_EQ(s->stateHash(), oldHash);
        CHECK_EQ(s->bookmarks().size(), size_t(2));
      }
    }
    s.reset();  // simulated crash right after the failing write

    std::unique_ptr<Session> r;
    Status os = reopen(dir, r);
    REQUIRE(os.ok());
    if (r->takeLength() == 0) {
      ++sawNew;
      checkEmpty(*r);
      CHECK(r->practiceSlot(0)->used);
    } else {
      ++sawOld;
      CHECK(r->timeline().flattenActive() == oldLog);
      CHECK_EQ(r->takes().size(), oldTakes);
      CHECK_EQ(r->bookmarks().size(), size_t(2));
      CHECK_EQ(r->frame(), oldFrame);
    }
    // Either way the project keeps working: record, save, reopen.
    auto more = DeterminismHarness::randomScript(60, 17);
    REQUIRE(r->seek(r->takeLength()).ok());
    REQUIRE(r->setMode(Mode::Record).ok());
    REQUIRE(recordScript(*r, more).ok());
    auto log = r->timeline().flattenActive();
    REQUIRE(r->save().ok());
    r.reset();
    REQUIRE(reopen(dir, r).ok());
    CHECK(r->timeline().flattenActive() == log);
  }
  CHECK(done);
  CHECK(sawOld > 0);
  CHECK(sawNew > 0);
  CHECK(rolledBack > 0);
}

TEST_CASE("reset: C API") {
  std::string root = tempDir("reset-capi");
  std::string rom = writeTestRom(root);
  rn_session_options o;
  rn_session_options_init(&o);
  o.core = RN_CORE_MOCK;
  rn_session* s = nullptr;
  REQUIRE_EQ(int(rn_session_new(rom.c_str(), fs::join(root, "a.nesrec").c_str(), &o, &s)), int(RN_OK));
  for (int i = 0; i < 200; ++i) REQUIRE_EQ(int(rn_step(s, uint8_t(i), 0, 0, nullptr)), int(RN_OK));
  REQUIRE_EQ(int(rn_practice_set_a(s, 1)), int(RN_OK));
  uint64_t id = 0;
  REQUIRE_EQ(int(rn_bookmark_add(s, "x", &id)), int(RN_OK));
  REQUIRE_EQ(int(rn_set_mode(s, RN_MODE_PRACTICE)), int(RN_OK));
  CHECK_EQ(int(rn_session_reset(s, 1)), int(RN_ERR_WRONG_MODE));
  REQUIRE_EQ(int(rn_set_mode(s, RN_MODE_REPLAY)), int(RN_OK));
  REQUIRE_EQ(int(rn_session_reset(s, 1)), int(RN_OK));
  CHECK_EQ(rn_frame(s), 0u);
  CHECK_EQ(rn_take_length(s), 0u);
  CHECK_EQ(rn_take_count(s), size_t(0));
  CHECK_EQ(rn_bookmark_count(s), size_t(0));
  CHECK_EQ(rn_undo_depth(s), size_t(0));
  CHECK_EQ(int(rn_get_mode(s)), int(RN_MODE_RECORD));
  CHECK_EQ(rn_session_has_unsaved_changes(s), 0);
  rn_practice_slot_info info;
  REQUIRE_EQ(int(rn_practice_slot_get(s, 1, &info)), int(RN_OK));
  CHECK(info.has_a);
  CHECK_EQ(info.has_take_frame, 0);
  REQUIRE_EQ(int(rn_session_reset(s, 0)), int(RN_OK));
  REQUIRE_EQ(int(rn_practice_slot_get(s, 1, &info)), int(RN_OK));
  CHECK_FALSE(info.has_a);
  CHECK_EQ(int(rn_session_reset(nullptr, 0)), int(RN_ERR_INVALID_ARG));
  rn_session_close(s);
}
