// Retry bursts (docs/design/VTR_UX.md): rn_take_discard / rn_take_truncate drop an abandoned
// attempt instead of keeping it as another take; persistence stays the existing format.
#include "persist/ProjectStore.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"

using namespace rn;
using namespace rntest;

namespace {
std::vector<InputRecord> script(size_t n, uint32_t seed) { return DeterminismHarness::randomScript(n, seed); }

std::unique_ptr<Session> createProject(const std::string& root, const std::string& dir, CoreKind kind) {
  std::string rom = writeTestRom(root, "retry.nes");
  std::vector<uint8_t> bytes;
  if (!fs::readFile(rom, bytes).ok()) return nullptr;
  SessionOptions o;
  o.core = kind;
  std::unique_ptr<Session> s;
  if (!Session::create(bytes, rom, o, s).ok()) return nullptr;
  if (!s->saveAs(dir).ok()) return nullptr;
  return s;
}

Status reopen(const std::string& dir, std::unique_ptr<Session>& out, OpenReport* rep = nullptr) {
  SessionOptions o;
  return ProjectStore::open(dir, "", o, out, rep);
}
}  // namespace

TEST_CASE("truncate: a retry inside the abandoned attempt continues it (no extra take)") {
  auto s = newSession(CoreKind::Nestopia, {30, 1200, 120});
  REQUIRE(s);
  auto a = script(600, 1), b = script(200, 2), c = script(150, 3);
  REQUIRE(recordScript(*s, a).ok());
  const uint64_t original = s->activeTake();
  REQUIRE(s->seek(300).ok());
  REQUIRE(recordScript(*s, b).ok());  // the attempt: frames 300..500 of a new take
  const uint64_t attempt = s->activeTake();
  CHECK(attempt != original);
  REQUIRE(s->rewind(100).ok());       // retry from 400, inside the attempt's own frames
  const uint64_t hash = s->stateHash();
  REQUIRE(s->truncateTake().ok());
  const uint64_t cut = s->activeTake();
  CHECK(cut != attempt);
  CHECK(s->timeline().segment(attempt) == nullptr);
  CHECK_EQ(s->frame(), 400u);
  CHECK_EQ(s->takeLength(), 400u);
  CHECK_EQ(s->stateHash(), hash);
  CHECK_EQ(s->takes().size(), 2u);
  StepInfo si;
  REQUIRE(s->step(c[0].p1, c[0].p2, 0, &si).ok());
  CHECK_FALSE(si.branched);
  REQUIRE(recordScript(*s, std::vector<InputRecord>(c.begin() + 1, c.end())).ok());
  CHECK_EQ(s->takes().size(), 2u);
  std::vector<InputRecord> expect(a.begin(), a.begin() + 300);
  expect.insert(expect.end(), b.begin(), b.begin() + 100);
  expect.insert(expect.end(), c.begin(), c.end());
  CHECK(s->timeline().flattenActive() == expect);
  const uint64_t end = s->stateHash();
  CHECK_EQ(straightReplay(CoreKind::Nestopia, expect, false, false).finalState, end);
  // Seeking back and forth over the copied checkpoints lands on the same states.
  REQUIRE(s->seek(350).ok());
  REQUIRE(s->seek(s->takeLength()).ok());
  CHECK_EQ(s->stateHash(), end);
  // The original continuation is untouched; "Back to Previous Take" names the copy.
  REQUIRE(s->activateTake(original).ok());
  CHECK_EQ(s->takeLength(), 600u);
  REQUIRE(s->undoTakeSwitch().ok());
  CHECK_EQ(s->activeTake(), cut);
}

TEST_CASE("truncate: refusals and no-ops") {
  auto s = newSession(CoreKind::Mock);
  REQUIRE(recordScript(*s, script(300, 4)).ok());
  CHECK(s->truncateTake().ok());  // at the take end: nothing to cut
  CHECK_EQ(s->takeLength(), 300u);
  REQUIRE(s->seek(100).ok());
  // The frames after the cursor belong to the root take's own segment, which starts at 0.
  const uint64_t root = s->activeTake();
  REQUIRE(s->truncateTake().ok());
  CHECK(s->activeTake() != root);
  CHECK_EQ(s->takeLength(), 100u);
  // A branch: the cursor at / before its branch frame is not inside its own frames.
  REQUIRE(recordScript(*s, script(100, 5)).ok());
  REQUIRE(s->seek(50).ok());
  REQUIRE(recordScript(*s, script(60, 6)).ok());  // new take from 50 (sibling)
  REQUIRE(s->seek(50).ok());
  CHECK_EQ(int(s->truncateTake().code), int(Err::InvalidArg));
  REQUIRE(s->seek(30).ok());
  CHECK_EQ(int(s->truncateTake().code), int(Err::InvalidArg));
  // A bookmark on the take keeps it.
  REQUIRE(s->seek(80).ok());
  uint64_t bm = 0;
  REQUIRE(s->bookmarkAdd("x", &bm).ok());
  REQUIRE(s->seek(70).ok());
  CHECK_EQ(int(s->truncateTake().code), int(Err::InvalidArg));
  REQUIRE(s->bookmarkRemove(bm).ok());
  CHECK(s->truncateTake().ok());
  CHECK_EQ(s->takeLength(), 70u);
  // Practice: wrong mode.
  REQUIRE(s->setMode(Mode::Practice).ok());
  CHECK_EQ(int(s->truncateTake().code), int(Err::WrongMode));
}

TEST_CASE("discard: an abandoned attempt before its branch frame is dropped") {
  auto s = newSession(CoreKind::Nestopia);
  REQUIRE(s);
  auto a = script(600, 7);
  REQUIRE(recordScript(*s, a).ok());
  const uint64_t original = s->activeTake();
  REQUIRE(s->seek(300).ok());
  REQUIRE(recordScript(*s, script(100, 8)).ok());
  const uint64_t attempt = s->activeTake();
  REQUIRE(s->seek(250).ok());  // retry before the attempt's branch frame
  CHECK_EQ(int(s->truncateTake().code), int(Err::InvalidArg));
  StepInfo si;
  REQUIRE(s->step(1, 0, 0, &si).ok());
  CHECK(si.branched);
  const uint64_t retry = s->activeTake();
  CHECK_EQ(int(s->discardTake(retry).code), int(Err::InvalidArg));  // active
  CHECK_EQ(int(s->discardTake(999).code), int(Err::NotFound));
  CHECK_EQ(int(s->discardTake(original).code), int(Err::InvalidArg));  // takes branch from it
  REQUIRE(s->discardTake(attempt).ok());
  CHECK_EQ(s->takes().size(), 2u);
  CHECK(s->timeline().segment(attempt) == nullptr);
  for (auto& kv : s->checkpoints().all()) CHECK(kv.second.owner != attempt);
  for (auto& u : s->timeline().undo()) CHECK(u.head != attempt);
  // Undo of a retry = back to the original continuation at its end, then drop the retry.
  REQUIRE(s->activateTake(original).ok());
  REQUIRE(s->seek(600).ok());
  REQUIRE(s->discardTake(retry).ok());
  CHECK_EQ(s->takes().size(), 1u);
  CHECK(s->timeline().flattenActive() == a);
  CHECK_EQ(s->stateHash(), straightReplay(CoreKind::Nestopia, a, false, false).finalState);
}

TEST_CASE("discard / truncate: saved, autosaved and crash-recovered projects stay valid") {
  std::string root = tempDir("retrytakes");
  std::string dir = fs::join(root, "Run.nesrec");
  auto s = createProject(root, dir, CoreKind::Nestopia);
  REQUIRE(s);
  REQUIRE(recordScript(*s, script(600, 9)).ok());
  REQUIRE(s->save().ok());
  REQUIRE(s->seek(300).ok());
  REQUIRE(recordScript(*s, script(200, 10)).ok());
  const uint64_t attempt = s->activeTake();
  REQUIRE(s->autosave().ok());  // the attempt is in the journal
  REQUIRE(s->rewind(100).ok());
  REQUIRE(s->truncateTake().ok());
  REQUIRE(recordScript(*s, script(50, 11)).ok());
  REQUIRE(s->seek(200).ok());
  REQUIRE(recordScript(*s, script(30, 12)).ok());  // another retry, before the cut take's start
  uint64_t abandoned = 0;
  for (auto& t : s->takes())
    if (!t.active && t.children == 0 && t.branchFrame == 300) abandoned = t.id;
  REQUIRE(abandoned != 0);
  REQUIRE(s->discardTake(abandoned).ok());
  REQUIRE(s->autosave().ok());
  const uint64_t head = s->activeTake(), frame = s->frame(), hash = s->stateHash();
  const size_t takes = s->takes().size();
  s.reset();  // crash: no full save since the attempt was journaled

  std::unique_ptr<Session> r;
  OpenReport rep;
  REQUIRE(reopen(dir, r, &rep).ok());
  CHECK(rep.journalApplied);
  CHECK_EQ(r->activeTake(), head);
  CHECK_EQ(r->frame(), frame);
  CHECK_EQ(r->stateHash(), hash);
  // The journaled attempt may come back as a hidden extra take; nothing else changes.
  CHECK(r->takes().size() >= takes);
  CHECK(r->timeline().segment(attempt) != nullptr);
  // A full save drops what was discarded for good.
  for (auto& t : r->takes())
    if (!t.active && t.children == 0 && t.id != r->takes().front().id) (void)r->discardTake(t.id);
  REQUIRE(r->save().ok());
  const size_t kept = r->takes().size();
  r.reset();
  std::unique_ptr<Session> q;
  REQUIRE(reopen(dir, q).ok());
  CHECK_EQ(q->takes().size(), kept);
  CHECK_EQ(q->activeTake(), head);
  CHECK_EQ(q->stateHash(), hash);
}
