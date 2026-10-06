// Practice mode (non-recording play) and per-project A/B repeat slots: take untouched, exact
// restore, A/B length, determinism of goto A, continuity rules, persistence, crash recovery,
// corruption handling, formatVersion 1 compatibility, C API.
#include <algorithm>
#include <cstring>
#include <functional>

#include "persist/ProjectStore.h"
#include "replaynes/replaynes.h"
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

Status reopen(const std::string& dir, std::unique_ptr<Session>& out, OpenReport* rep = nullptr,
              bool dropStates = false, bool dropPractice = false) {
  SessionOptions o;
  o.dropCorruptStates = dropStates;
  o.dropCorruptPractice = dropPractice;
  return ProjectStore::open(dir, "", o, out, rep);
}

// Steps the session (any mode) with a script; returns per-frame video hashes.
std::vector<uint64_t> play(Session& s, const std::vector<InputRecord>& recs) {
  std::vector<uint64_t> v;
  for (auto& r : recs) {
    if (!s.step(r.p1, r.p2, r.events).ok()) return {};
    v.push_back(s.videoHash());
  }
  return v;
}

std::string practiceFile(const std::string& dir, int slot) {
  std::vector<std::string> names;
  fs::listDir(fs::join(dir, "states"), names);
  std::string prefix = "practice-" + std::to_string(slot) + "-";
  for (auto& n : names)
    if (n.compare(0, prefix.size(), prefix) == 0) return fs::join(dir, "states/" + n);
  return "";
}
}  // namespace

TEST_CASE("practice: steps record nothing; leaving restores the exact take state (nestopia)") {
  std::string root = tempDir("prac-take");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "p.nesrec");
  auto s = createProject(dir, rom, CoreKind::Nestopia);
  REQUIRE(s);
  auto a = DeterminismHarness::randomScript(500, 41, 200, 3);
  REQUIRE(recordScript(*s, a).ok());
  REQUIRE(s->seek(300).ok());
  REQUIRE(s->save().ok());
  CHECK_FALSE(s->unsavedChanges());
  const uint64_t hash0 = s->stateHash(), len0 = s->takeLength(), head0 = s->activeTake();
  const auto log0 = s->timeline().flattenActive();
  const size_t takes0 = s->takes().size(), cps0 = s->checkpoints().all().size();

  REQUIRE(s->setMode(Mode::Practice).ok());
  CHECK(s->mode() == Mode::Practice);
  CHECK_EQ(s->practiceCounter(), 0u);
  StepInfo si;
  auto b = DeterminismHarness::randomScript(400, 42, 150, 2);
  for (auto& r : b) REQUIRE(s->step(r.p1, r.p2, r.events, &si).ok());
  CHECK(si.mode == Mode::Practice);
  CHECK_FALSE(si.branched);
  CHECK_EQ(si.frame, 300u);
  CHECK_EQ(s->practiceCounter(), 400u);
  CHECK_EQ(s->frame(), 300u);
  CHECK_EQ(s->takeLength(), len0);
  CHECK_EQ(s->activeTake(), head0);
  CHECK(s->timeline().flattenActive() == log0);
  CHECK_EQ(s->takes().size(), takes0);
  CHECK_EQ(s->checkpoints().all().size(), cps0);
  CHECK_FALSE(s->unsavedChanges());
  CHECK(s->stateHash() != hash0);
  // Take navigation is refused while practicing.
  CHECK_EQ(int(s->seek(10).code), int(Err::WrongMode));
  uint64_t bm;
  CHECK_EQ(int(s->bookmarkAdd("x", &bm).code), int(Err::WrongMode));
  CHECK_EQ(int(s->activateTake(head0).code), int(Err::WrongMode));
  CHECK_EQ(int(s->undoTakeSwitch().code), int(Err::WrongMode));

  REQUIRE(s->setMode(Mode::Record).ok());
  CHECK(s->mode() == Mode::Record);
  CHECK_EQ(s->frame(), 300u);
  CHECK_EQ(s->stateHash(), hash0);
  CHECK_FALSE(s->unsavedChanges());
  size_t n = 1;
  s->audio(&n);
  CHECK_EQ(n, size_t(0));
  // The take continues exactly as if practice never happened.
  auto c = DeterminismHarness::randomScript(100, 43);
  REQUIRE(recordScript(*s, c).ok());
  auto log = s->timeline().flattenActive();
  REQUIRE_EQ(log.size(), size_t(400));
  CHECK(std::equal(c.begin(), c.end(), log.begin() + 300));
  CHECK_EQ(s->stateHash(), straightReplay(CoreKind::Nestopia, log).finalState);
}

TEST_CASE("practice: A/B length and goto A reproduces identical frames (nestopia)") {
  auto s = newSession(CoreKind::Nestopia);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(300, 51)).ok());
  REQUIRE(s->practiceSetA(0).ok());
  const uint64_t hashA = s->stateHash();
  auto b = DeterminismHarness::randomScript(200, 52, 120, 2);
  auto vTake = play(*s, b);  // recorded on the take
  REQUIRE(s->practiceSetB(0).ok());
  const uint64_t hashB = s->stateHash();
  const PracticeSlot* p = s->practiceSlot(0);
  REQUIRE(p);
  CHECK(p->used);
  CHECK(p->hasB);
  CHECK_EQ(p->length, 200u);
  CHECK(p->hasTakeFrame);
  CHECK_EQ(p->takeFrame, 300u);
  CHECK_EQ(p->takeId, s->activeTake());

  REQUIRE(s->practiceGotoA(0).ok());
  CHECK(s->mode() == Mode::Practice);
  CHECK_EQ(s->practiceCounter(), 0u);
  CHECK_EQ(s->stateHash(), hashA);
  auto v1 = play(*s, b);
  CHECK_EQ(s->practiceCounter(), 200u);
  CHECK_EQ(s->stateHash(), hashB);
  CHECK(v1 == vTake);
  REQUIRE(s->practiceGotoA(0).ok());
  auto v2 = play(*s, b);
  CHECK(v2 == v1);
  CHECK_EQ(s->practiceStatus().anchorSlot, 0);
  // Re-defining B inside practice (continuous since goto A).
  REQUIRE(s->practiceGotoA(0).ok());
  play(*s, std::vector<InputRecord>(b.begin(), b.begin() + 77));
  REQUIRE(s->practiceSetB(0).ok());
  CHECK_EQ(s->practiceSlot(0)->length, 77u);
  // Leaving practice returns to the take cursor (frame 500).
  REQUIRE(s->setMode(Mode::Replay).ok());
  CHECK_EQ(s->frame(), 500u);
  CHECK_EQ(s->stateHash(), hashB);
}

TEST_CASE("practice: continuity rules, slot limit, practice rewind") {
  auto s = newSession(CoreKind::Mock);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(100, 61)).ok());
  REQUIRE(s->practiceSetA(1).ok());
  CHECK_EQ(int(s->practiceSetB(1).code), int(Err::InvalidArg));  // zero length
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(10, 62)).ok());
  CHECK(s->practiceBSettable(1));
  REQUIRE(s->seek(105).ok());
  CHECK_FALSE(s->practiceBSettable(1));
  CHECK_EQ(int(s->practiceSetB(1).code), int(Err::Discontinuity));
  CHECK_EQ(s->practiceCounter(), 0u);
  CHECK_EQ(int(s->practiceSetB(2).code), int(Err::NotFound));
  CHECK_EQ(int(s->practiceGotoA(2).code), int(Err::NotFound));
  CHECK_EQ(int(s->practiceSetA(8).code), int(Err::OutOfRange));
  CHECK_EQ(int(s->practiceSetA(-1).code), int(Err::OutOfRange));
  CHECK_EQ(int(s->practiceGotoA(8).code), int(Err::OutOfRange));
  for (int i = 0; i < kPracticeSlots; ++i) REQUIRE(s->practiceSetA(i).ok());
  // A take switch breaks continuity too.
  REQUIRE(s->practiceSetA(1).ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(5, 63)).ok());  // branch at 105
  REQUIRE(s->undoTakeSwitch().ok());
  CHECK_EQ(int(s->practiceSetB(1).code), int(Err::Discontinuity));
  // Anchor set on the take stays live when entering practice (continuous).
  REQUIRE(s->practiceSetA(4).ok());
  REQUIRE(s->setMode(Mode::Practice).ok());
  play(*s, DeterminismHarness::randomScript(12, 64));
  REQUIRE(s->practiceSetB(4).ok());
  CHECK_EQ(s->practiceSlot(4)->length, 12u);
  // A set inside a practice run has no take frame.
  REQUIRE(s->practiceSetA(3).ok());
  CHECK_FALSE(s->practiceSlot(3)->hasTakeFrame);

  // Practice rewind: exact, deterministic, clamped at the anchor, keeps continuity.
  auto run = DeterminismHarness::randomScript(100, 65);
  std::vector<uint64_t> st;
  for (auto& r : run) {
    REQUIRE(s->step(r.p1, r.p2, r.events).ok());
    st.push_back(s->stateHash());
  }
  CHECK_EQ(s->practiceCounter(), 100u);
  CHECK_EQ(s->practiceStatus().rewindAvailable, 100u);
  REQUIRE(s->rewind(37).ok());
  CHECK_EQ(s->practiceCounter(), 63u);
  CHECK_EQ(s->stateHash(), st[62]);
  CHECK_EQ(s->frame(), 105u);  // take cursor untouched (undo returned to 105)
  for (size_t i = 63; i < run.size(); ++i) {
    REQUIRE(s->step(run[i].p1, run[i].p2, run[i].events).ok());
    CHECK_EQ(s->stateHash(), st[i]);
  }
  REQUIRE(s->rewind(31).ok());  // not on a snapshot boundary
  CHECK_EQ(s->stateHash(), st[68]);
  REQUIRE(s->practiceSetB(3).ok());
  CHECK_EQ(s->practiceSlot(3)->length, 69u);
  REQUIRE(s->rewind(100000).ok());  // clamps at A of slot 3
  CHECK_EQ(s->practiceCounter(), 0u);
  REQUIRE(s->practiceGotoA(3).ok());
  uint64_t hashA3 = s->stateHash();
  REQUIRE(s->rewind(5).ok());  // nothing before A
  CHECK_EQ(s->stateHash(), hashA3);
  // Clearing a slot.
  REQUIRE(s->practiceClear(3).ok());
  CHECK_FALSE(s->practiceSlot(3)->used);
  CHECK_EQ(int(s->practiceGotoA(3).code), int(Err::NotFound));
  CHECK_EQ(int(s->practiceRename(3, "x").code), int(Err::NotFound));
}

TEST_CASE("practice: slots survive save/close/reopen; goto A works; save while practicing (nestopia)") {
  std::string root = tempDir("prac-save");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "s.nesrec");
  auto s = createProject(dir, rom, CoreKind::Nestopia);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(200, 71)).ok());
  REQUIRE(s->practiceSetA(2).ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(100, 72)).ok());
  REQUIRE(s->practiceSetB(2).ok());
  REQUIRE(s->practiceRename(2, "ボス前").ok());
  REQUIRE(s->practiceSetA(7).ok());  // A only, at frame 300
  auto b = DeterminismHarness::randomScript(50, 73);
  REQUIRE(s->practiceGotoA(2).ok());
  auto v = play(*s, b);
  const uint64_t h50 = s->stateHash();
  REQUIRE(s->setMode(Mode::Record).ok());
  const uint64_t takeHash = s->stateHash();
  PracticeSlot before = *s->practiceSlot(2);
  // Save while practicing: the cursor/head state saved is the take's, not the practice run's.
  REQUIRE(s->practiceGotoA(7).ok());
  play(*s, b);
  REQUIRE(s->save().ok());
  CHECK_FALSE(s->unsavedChanges());
  CHECK(fs::exists(fs::join(dir, "metadata/practice.json")));
  s.reset();

  std::unique_ptr<Session> r;
  OpenReport rep;
  REQUIRE(reopen(dir, r, &rep).ok());
  CHECK_FALSE(rep.journalApplied);
  CHECK_FALSE(r->unsavedChanges());
  CHECK(r->mode() == Mode::Record);  // practice is never persisted
  CHECK_EQ(r->frame(), 300u);
  CHECK_EQ(r->stateHash(), takeHash);
  const PracticeSlot* p = r->practiceSlot(2);
  REQUIRE(p);
  REQUIRE(p->used);
  CHECK_EQ(p->name, std::string("ボス前"));
  CHECK(p->hasB);
  CHECK_EQ(p->length, 100u);
  CHECK(p->hasTakeFrame);
  CHECK_EQ(p->takeFrame, 200u);
  CHECK_EQ(p->createdSeq, before.createdSeq);
  CHECK_EQ(p->updatedSeq, before.updatedSeq);
  CHECK(r->practiceSlot(7)->used);
  CHECK_FALSE(r->practiceSlot(7)->hasB);
  CHECK_FALSE(r->practiceSlot(0)->used);
  CHECK_FALSE(r->practiceBSettable(2));  // no live anchor after reopening
  CHECK_EQ(int(r->practiceSetB(7).code), int(Err::Discontinuity));
  REQUIRE(r->practiceGotoA(2).ok());
  CHECK(play(*r, b) == v);
  CHECK_EQ(r->stateHash(), h50);
  CHECK_FALSE(r->unsavedChanges());
  // New slots after reopening get higher sequence numbers.
  REQUIRE(r->practiceSetA(5).ok());
  CHECK(r->practiceSlot(5)->createdSeq > before.updatedSeq);
  // Re-setting A replaces the state file; the old one is garbage-collected on the next save.
  REQUIRE(r->setMode(Mode::Record).ok());
  REQUIRE(r->practiceSetA(2).ok());
  REQUIRE(r->save().ok());
  std::vector<std::string> names;
  fs::listDir(fs::join(dir, "states"), names);
  int practiceFiles = 0;
  for (auto& n : names) practiceFiles += n.compare(0, 9, "practice-") == 0;
  CHECK_EQ(practiceFiles, 3);
}

TEST_CASE("practice: crash recovery via the journal keeps slots") {
  std::string root = tempDir("prac-crash");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "c.nesrec");
  auto s = createProject(dir, rom, CoreKind::Mock);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(300, 81)).ok());
  REQUIRE(s->practiceSetA(0).ok());
  REQUIRE(s->save().ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(40, 82)).ok());
  REQUIRE(s->practiceSetB(0).ok());
  REQUIRE(s->practiceGotoA(0).ok());
  play(*s, DeterminismHarness::randomScript(25, 83));
  REQUIRE(s->practiceSetA(6).ok());  // A inside practice, after the last full save
  const uint64_t hashA6 = s->stateHash();
  REQUIRE(s->practiceRename(6, "practice A"));
  REQUIRE(s->autosave().ok());
  REQUIRE(s->practiceSetA(5).ok());  // never autosaved: lost
  const uint64_t takeFrame = s->frame();
  s.reset();  // kill

  std::unique_ptr<Session> r;
  OpenReport rep;
  REQUIRE(reopen(dir, r, &rep).ok());
  CHECK(rep.journalApplied);
  CHECK(r->recovered());
  CHECK_EQ(r->frame(), takeFrame);
  CHECK(r->mode() == Mode::Record);
  REQUIRE(r->practiceSlot(0)->used);
  CHECK_EQ(r->practiceSlot(0)->length, 40u);
  REQUIRE(r->practiceSlot(6)->used);
  CHECK_EQ(r->practiceSlot(6)->name, std::string("practice A"));
  CHECK_FALSE(r->practiceSlot(5)->used);
  REQUIRE(r->practiceGotoA(6).ok());
  CHECK_EQ(r->stateHash(), hashA6);
  // Folded into a full save; reopening without a journal keeps them.
  REQUIRE(r->save().ok());
  r.reset();
  REQUIRE(reopen(dir, r, &rep).ok());
  CHECK_FALSE(rep.journalApplied);
  REQUIRE(r->practiceSlot(6)->used);
  REQUIRE(r->practiceGotoA(6).ok());
  CHECK_EQ(r->stateHash(), hashA6);
}

TEST_CASE("practice: corrupt slot is an explicit error; dropped only with its own flag") {
  std::string root = tempDir("prac-corrupt");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "x.nesrec");
  auto s = createProject(dir, rom, CoreKind::Nestopia);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(120, 91)).ok());
  REQUIRE(s->practiceSetA(0).ok());
  const uint64_t hashA0 = s->stateHash();
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(30, 92)).ok());
  REQUIRE(s->practiceSetA(3).ok());
  REQUIRE(s->save().ok());
  s.reset();
  std::string f3 = practiceFile(dir, 3);
  REQUIRE(!f3.empty());
  std::vector<uint8_t> good;
  fs::readFile(f3, good);
  std::vector<uint8_t> bad = good;
  bad[bad.size() / 2] ^= 0x5A;
  fs::writeFileAtomic(f3, bad.data(), bad.size());

  std::unique_ptr<Session> r;
  Status st = reopen(dir, r);
  CHECK_EQ(int(st.code), int(Err::Corrupt));
  CHECK(st.message.find("practice") != std::string::npos);
  CHECK(st.message.find("drop-corrupt-practice") != std::string::npos);
  CHECK_EQ(int(reopen(dir, r, nullptr, true, false).code), int(Err::Corrupt));  // states flag is not enough
  OpenReport rep;
  REQUIRE(reopen(dir, r, &rep, false, true).ok());
  CHECK_EQ(rep.droppedPracticeSlots.size(), size_t(1));
  CHECK_EQ(r->droppedPracticeSlots(), 1u << 3);
  CHECK(r->unsavedChanges());
  CHECK_FALSE(r->practiceSlot(3)->used);
  REQUIRE(r->practiceSlot(0)->used);
  REQUIRE(r->practiceGotoA(0).ok());
  CHECK_EQ(r->stateHash(), hashA0);
  r.reset();
  // Missing file is corruption too.
  fs::removeFile(f3);
  CHECK_EQ(int(reopen(dir, r).code), int(Err::Corrupt));
  // Index entry checksum disagreeing with an intact file.
  fs::writeFileAtomic(f3, good.data(), good.size());
  REQUIRE(reopen(dir, r).ok());
  r.reset();
  std::string ip = fs::join(dir, "timeline/index.json"), text;
  fs::readText(ip, text);
  size_t pos = text.find("\"crc32\":", text.find("\"practice\""));
  REQUIRE(pos != std::string::npos);
  text.insert(pos + 8, "1");
  fs::writeFileAtomic(ip, text);
  CHECK_EQ(int(reopen(dir, r).code), int(Err::Corrupt));
}

TEST_CASE("practice: formatVersion 1 projects (no practice data) still open") {
  std::string root = tempDir("prac-v1");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "v1.nesrec");
  auto s = createProject(dir, rom, CoreKind::Mock);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(200, 101)).ok());
  REQUIRE(s->save().ok());
  const uint64_t hash = s->stateHash();
  s.reset();
  // Rewrite as a version-1 project: no meta.practice, no practice mirror.
  auto rewrite = [&](const std::string& rel, const std::function<void(Json&)>& f) {
    std::string t;
    fs::readText(fs::join(dir, rel), t);
    Json j;
    REQUIRE(Json::parse(t, j));
    f(j);
    fs::writeFileAtomic(fs::join(dir, rel), j.dump());
  };
  rewrite("manifest.json", [](Json& j) { j.set("formatVersion", int64_t(1)); });
  rewrite("timeline/index.json", [](Json& j) {
    Json meta = Json::object();
    for (auto& kv : j["meta"].members())
      if (kv.first != "practice") meta.set(kv.first, kv.second);
    j.set("meta", meta);
  });
  fs::removeFile(fs::join(dir, "metadata/practice.json"));
  std::unique_ptr<Session> r;
  REQUIRE(reopen(dir, r).ok());
  CHECK_EQ(r->stateHash(), hash);
  for (int i = 0; i < kPracticeSlots; ++i) CHECK_FALSE(r->practiceSlot(i)->used);
  REQUIRE(r->practiceSetA(0).ok());
  REQUIRE(r->save().ok());
  std::string man;
  fs::readText(fs::join(dir, "manifest.json"), man);
  Json mj;
  Json::parse(man, mj);
  CHECK_EQ(mj["formatVersion"].asInt(), int64_t(kProjectFormatVersion));
  CHECK_EQ(kProjectFormatVersion, 2u);
}

TEST_CASE("practice: C API") {
  std::string root = tempDir("prac-capi");
  std::string rom = writeTestRom(root);
  rn_session_options o;
  rn_session_options_init(&o);
  o.core = RN_CORE_MOCK;
  rn_session* s = nullptr;
  REQUIRE_EQ(int(rn_session_new(rom.c_str(), fs::join(root, "a.nesrec").c_str(), &o, &s)), int(RN_OK));
  for (int i = 0; i < 60; ++i) REQUIRE_EQ(int(rn_step(s, uint8_t(i), 0, 0, nullptr)), int(RN_OK));
  REQUIRE_EQ(int(rn_session_save(s)), int(RN_OK));
  REQUIRE_EQ(int(rn_practice_set_a(s, 1)), int(RN_OK));
  for (int i = 0; i < 30; ++i) REQUIRE_EQ(int(rn_step(s, 1, 0, 0, nullptr)), int(RN_OK));
  CHECK_EQ(rn_practice_frame(s), 30u);
  REQUIRE_EQ(int(rn_practice_set_b(s, 1)), int(RN_OK));
  REQUIRE_EQ(int(rn_practice_slot_rename(s, 1, "loop")), int(RN_OK));
  rn_practice_slot_info info;
  REQUIRE_EQ(int(rn_practice_slot_get(s, 1, &info)), int(RN_OK));
  CHECK_EQ(info.has_a, 1);
  CHECK_EQ(info.has_b, 1);
  CHECK_EQ(info.length_frames, 30u);
  CHECK_EQ(std::string(info.name), std::string("loop"));
  CHECK_EQ(info.has_take_frame, 1);
  CHECK_EQ(info.take_frame, 60u);
  CHECK_EQ(info.b_settable, 1);
  REQUIRE_EQ(int(rn_practice_slot_get(s, 0, &info)), int(RN_OK));
  CHECK_EQ(info.has_a, 0);
  CHECK_EQ(int(rn_practice_slot_get(s, RN_PRACTICE_SLOTS, &info)), int(RN_ERR_OUT_OF_RANGE));
  CHECK_EQ(int(rn_practice_set_a(s, 8)), int(RN_ERR_OUT_OF_RANGE));

  REQUIRE_EQ(int(rn_practice_goto_a(s, 1)), int(RN_OK));
  CHECK_EQ(int(rn_get_mode(s)), int(RN_MODE_PRACTICE));
  rn_step_info si;
  REQUIRE_EQ(int(rn_step(s, 2, 0, 0, &si)), int(RN_OK));
  CHECK_EQ(int(si.mode), int(RN_MODE_PRACTICE));
  CHECK_EQ(si.frame, 90u);
  rn_practice_status ps;
  REQUIRE_EQ(int(rn_practice_get_status(s, &ps)), int(RN_OK));
  CHECK_EQ(ps.active, 1);
  CHECK_EQ(ps.anchor_slot, 1);
  CHECK_EQ(ps.counter, 1u);
  CHECK_EQ(ps.return_frame, 90u);
  CHECK_EQ(ps.rewind_available, 1u);
  CHECK_EQ(int(rn_seek(s, 0)), int(RN_ERR_WRONG_MODE));
  REQUIRE_EQ(int(rn_rewind(s, 1)), int(RN_OK));
  CHECK_EQ(rn_practice_frame(s), 0u);
  CHECK_EQ(rn_take_length(s), 90u);
  REQUIRE_EQ(int(rn_set_mode(s, RN_MODE_REPLAY)), int(RN_OK));
  CHECK_EQ(rn_frame(s), 90u);
  REQUIRE_EQ(int(rn_practice_set_a(s, 2)), int(RN_OK));
  REQUIRE_EQ(int(rn_seek(s, 10)), int(RN_OK));
  CHECK_EQ(int(rn_practice_set_b(s, 2)), int(RN_ERR_DISCONTINUITY));
  CHECK_EQ(std::string(rn_status_name(RN_ERR_DISCONTINUITY)), std::string("discontinuity"));
  REQUIRE_EQ(int(rn_practice_slot_clear(s, 2)), int(RN_OK));
  REQUIRE_EQ(int(rn_practice_slot_get(s, 2, &info)), int(RN_OK));
  CHECK_EQ(info.has_a, 0);
  CHECK_EQ(rn_practice_dropped_slots(s), 0u);
  REQUIRE_EQ(int(rn_session_save(s)), int(RN_OK));
  rn_session_close(s);
  s = nullptr;
  REQUIRE_EQ(int(rn_session_open(fs::join(root, "a.nesrec").c_str(), nullptr, &o, &s)), int(RN_OK));
  REQUIRE_EQ(int(rn_practice_slot_get(s, 1, &info)), int(RN_OK));
  CHECK_EQ(info.length_frames, 30u);
  CHECK_EQ(info.b_settable, 0);
  REQUIRE_EQ(int(rn_practice_goto_a(s, 1)), int(RN_OK));
  rn_session_close(s);
}
