// A/B slots defined directly from take frames (timeline selection): rn_practice_set_range /
// Session::practiceSetRange. Exact cursor restore, determinism of goto A vs. the take,
// persistence (full save + journal), errors, C API.
#include <utility>

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

std::vector<uint64_t> play(Session& s, const std::vector<InputRecord>& recs) {
  std::vector<uint64_t> v;
  for (auto& r : recs) {
    if (!s.step(r.p1, r.p2, r.events).ok()) return {};
    v.push_back(s.videoHash());
  }
  return v;
}

// Video hashes of the take frames emulated while replaying the take from cursor a to cursor b.
std::vector<uint64_t> takeVideo(Session& s, uint64_t a, uint64_t b) {
  std::vector<uint64_t> v;
  if (!s.seek(a).ok()) return v;
  for (uint64_t f = a; f < b; ++f) {
    if (!s.step(0, 0, 0).ok()) return {};
    v.push_back(s.videoHash());
  }
  return v;
}

struct RangeCase {
  int slot;
  uint64_t a, b;
};
}  // namespace

TEST_CASE("practice: set_range defines A/B from take frames; cursor restored exactly") {
  for (CoreKind kind : {CoreKind::Nestopia, CoreKind::Mock}) {
    auto s = newSession(kind);
    REQUIRE(s);
    REQUIRE(recordScript(*s, DeterminismHarness::randomScript(700, 91, 200, 3)).ok());
    REQUIRE(s->setMode(Mode::Replay).ok());
    REQUIRE(s->seek(500).ok());
    REQUIRE(s->practiceSetA(3).ok());  // live anchor: must survive set_range
    for (int i = 0; i < 20; ++i) REQUIRE(s->step(0, 0, 0).ok());
    REQUIRE(s->practiceBSettable(3));
    const uint64_t state0 = s->stateHash(), video0 = s->videoHash(), len0 = s->takeLength();
    const auto log = s->timeline().flattenActive();
    const size_t takes0 = s->takes().size();

    // Mid-take (checkpoint walk), frame 0 (fresh power-on walk), at the cursor, at the take end.
    const RangeCase cases[] = {{0, 100, 350}, {1, 0, 10}, {4, 520, 600}, {2, 650, 700}};
    for (const RangeCase& c : cases) {
      REQUIRE(s->practiceSetRange(c.slot, c.a, c.b).ok());
      CHECK_EQ(s->frame(), 520u);
      CHECK_EQ(s->stateHash(), state0);
      CHECK_EQ(s->videoHash(), video0);
      CHECK(s->mode() == Mode::Replay);
      CHECK(s->practiceBSettable(3));  // emulation continuity of other anchors kept
      size_t n = 1;
      s->audio(&n);
      CHECK_EQ(n, 0u);
      const PracticeSlot* p = s->practiceSlot(c.slot);
      REQUIRE(p);
      CHECK(p->used);
      CHECK(p->hasB);
      CHECK_EQ(p->length, c.b - c.a);
      CHECK(p->hasTakeFrame);
      CHECK_EQ(p->takeFrame, c.a);
      CHECK_EQ(p->takeId, s->activeTake());
      CHECK_FALSE(s->practiceBSettable(c.slot));
    }
    CHECK_EQ(s->takeLength(), len0);
    CHECK_EQ(s->takes().size(), takes0);
    CHECK(s->timeline().flattenActive() == log);
    CHECK(s->unsavedChanges());
    // The live anchor still measures B after all that.
    REQUIRE(s->practiceSetB(3).ok());
    CHECK_EQ(s->practiceSlot(3)->length, 20u);

    // Determinism: goto A + the take's inputs == the take's own frames, ending in the B state.
    for (const RangeCase& c : cases) {
      auto vTake = takeVideo(*s, c.a, c.b);
      REQUIRE_EQ(vTake.size(), size_t(c.b - c.a));
      const uint64_t hashB = s->stateHash();
      REQUIRE(s->seek(c.a).ok());
      const uint64_t hashA = s->stateHash();
      REQUIRE(s->practiceGotoA(c.slot).ok());
      CHECK_EQ(s->stateHash(), hashA);
      auto v = play(*s, std::vector<InputRecord>(log.begin() + ptrdiff_t(c.a), log.begin() + ptrdiff_t(c.b)));
      CHECK(v == vTake);
      CHECK_EQ(s->stateHash(), hashB);
      CHECK_EQ(s->practiceCounter(), c.b - c.a);
      REQUIRE(s->setMode(Mode::Replay).ok());
    }

    // Errors.
    CHECK_EQ(int(s->practiceSetRange(0, 10, 10).code), int(Err::InvalidArg));
    CHECK_EQ(int(s->practiceSetRange(0, 20, 10).code), int(Err::InvalidArg));
    CHECK_EQ(int(s->practiceSetRange(0, 10, 701).code), int(Err::OutOfRange));
    CHECK_EQ(int(s->practiceSetRange(kPracticeSlots, 10, 20).code), int(Err::OutOfRange));
    CHECK_EQ(s->practiceSlot(0)->takeFrame, 100u);  // failed calls change nothing
    REQUIRE(s->practiceGotoA(0).ok());
    CHECK_EQ(int(s->practiceSetRange(0, 10, 20).code), int(Err::WrongMode));
    REQUIRE(s->setMode(Mode::Record).ok());
    // Record mode, a == cursor: the current state is taken directly; nothing is recorded.
    REQUIRE(s->seek(300).ok());
    const uint64_t hc = s->stateHash(), vc = s->videoHash();
    REQUIRE(s->practiceSetRange(5, 300, 330).ok());
    CHECK_EQ(s->stateHash(), hc);
    CHECK_EQ(s->videoHash(), vc);
    CHECK_EQ(s->frame(), 300u);
    CHECK(s->mode() == Mode::Record);
    CHECK(s->timeline().flattenActive() == log);
    // Cursor at frame 0 (a fresh power-on), range further on.
    REQUIRE(s->seek(0).ok());
    const uint64_t h0 = s->stateHash(), v0 = s->videoHash();
    REQUIRE(s->practiceSetRange(5, 200, 260).ok());
    CHECK_EQ(s->frame(), 0u);
    CHECK_EQ(s->stateHash(), h0);
    CHECK_EQ(s->videoHash(), v0);
  }
}

TEST_CASE("practice: set_range slots persist (full save + journal) (nestopia)") {
  std::string root = tempDir("prac-range");
  std::string rom = writeTestRom(root);
  std::string dir = fs::join(root, "r.nesrec");
  auto s = createProject(dir, rom, CoreKind::Nestopia);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(400, 95)).ok());
  REQUIRE(s->practiceSetRange(0, 120, 300).ok());
  REQUIRE(s->practiceRename(0, "range").ok());
  REQUIRE(s->save().ok());
  REQUIRE(s->practiceSetRange(6, 30, 90).ok());  // only in the journal
  REQUIRE(s->autosave().ok());
  const auto log = s->timeline().flattenActive();
  REQUIRE(s->seek(120).ok());
  const uint64_t hashA = s->stateHash();
  REQUIRE(s->seek(30).ok());
  const uint64_t hashA6 = s->stateHash();
  s.reset();  // kill: no final save

  std::unique_ptr<Session> r;
  OpenReport rep;
  SessionOptions o;
  REQUIRE(ProjectStore::open(dir, "", o, r, &rep).ok());
  CHECK(rep.journalApplied);
  for (auto c : {std::pair<int, uint64_t>{0, 120}, {6, 30}}) {
    const PracticeSlot* p = r->practiceSlot(c.first);
    REQUIRE(p);
    REQUIRE(p->used);
    CHECK(p->hasB);
    CHECK(p->hasTakeFrame);
    CHECK_EQ(p->takeFrame, c.second);
    CHECK_EQ(p->length, c.first == 0 ? 180u : 60u);
    CHECK_EQ(p->takeId, r->activeTake());
  }
  CHECK_EQ(r->practiceSlot(0)->name, std::string("range"));
  REQUIRE(r->practiceGotoA(0).ok());
  CHECK_EQ(r->stateHash(), hashA);
  REQUIRE(r->practiceGotoA(6).ok());
  CHECK_EQ(r->stateHash(), hashA6);
  auto v = play(*r, std::vector<InputRecord>(log.begin() + 30, log.begin() + 90));
  REQUIRE(r->setMode(Mode::Replay).ok());
  CHECK(takeVideo(*r, 30, 90) == v);
}

TEST_CASE("practice: rn_practice_set_range C API") {
  std::string root = tempDir("prac-range-capi");
  std::string rom = writeTestRom(root);
  rn_session_options o;
  rn_session_options_init(&o);
  o.core = RN_CORE_MOCK;
  rn_session* s = nullptr;
  REQUIRE_EQ(int(rn_session_new(rom.c_str(), nullptr, &o, &s)), int(RN_OK));
  for (int i = 0; i < 90; ++i) REQUIRE_EQ(int(rn_step(s, uint8_t(i), 0, 0, nullptr)), int(RN_OK));
  REQUIRE_EQ(int(rn_seek(s, 70)), int(RN_OK));
  const uint64_t h = rn_state_hash(s);
  REQUIRE_EQ(int(rn_practice_set_range(s, 3, 10, 40)), int(RN_OK));
  CHECK_EQ(rn_frame(s), 70u);
  CHECK_EQ(rn_state_hash(s), h);
  rn_practice_slot_info info;
  REQUIRE_EQ(int(rn_practice_slot_get(s, 3, &info)), int(RN_OK));
  CHECK_EQ(info.has_a, 1);
  CHECK_EQ(info.has_b, 1);
  CHECK_EQ(info.length_frames, 30u);
  CHECK_EQ(info.has_take_frame, 1);
  CHECK_EQ(info.take_frame, 10u);
  CHECK_EQ(info.take_id, rn_active_take(s));
  CHECK_EQ(int(rn_practice_set_range(s, 3, 40, 40)), int(RN_ERR_INVALID_ARG));
  CHECK_EQ(int(rn_practice_set_range(s, 3, 0, 91)), int(RN_ERR_OUT_OF_RANGE));
  CHECK_EQ(int(rn_practice_set_range(s, RN_PRACTICE_SLOTS, 0, 1)), int(RN_ERR_OUT_OF_RANGE));
  CHECK_EQ(int(rn_practice_set_range(nullptr, 0, 0, 1)), int(RN_ERR_INVALID_ARG));
  REQUIRE_EQ(int(rn_practice_goto_a(s, 3)), int(RN_OK));
  CHECK_EQ(int(rn_practice_set_range(s, 3, 0, 1)), int(RN_ERR_WRONG_MODE));
  rn_session_close(s);
}
