#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <memory>
#include <vector>

#include "core/MockCore.h"
#include "replaynes/replaynes.h"
#include "persist/ProjectStore.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"

using namespace rn;
using namespace rntest;

namespace {
struct Counts { uint64_t steps = 0; std::vector<uint64_t> loads; };
class CountingCore final : public ICore {
 public:
  explicit CountingCore(std::shared_ptr<Counts> c) : counts_(std::move(c)) {}
  Status loadROM(const uint8_t* p, size_t n) override { return core_.loadROM(p, n); }
  Status powerCycle() override { return core_.powerCycle(); }
  Status softReset() override { return core_.softReset(); }
  Status stepFrame(uint8_t a, uint8_t b, bool render) override {
    ++counts_->steps;
    return core_.stepFrame(a, b, render);
  }
  uint64_t frameIndex() const override { return core_.frameIndex(); }
  const uint32_t* video() const override { return core_.video(); }
  const int16_t* audio(size_t* n) const override { return core_.audio(n); }
  Status saveState(std::vector<uint8_t>& out) override { return core_.saveState(out); }
  Status loadState(const uint8_t* p, size_t n) override {
    Status result = core_.loadState(p, n);
    if (result.ok()) counts_->loads.push_back(core_.frameIndex());
    return result;
  }
  std::string compatId() const override { return "counting-rewind-test/1"; }
  std::string buildId() const override { return "counting-rewind-test"; }
  uint32_t stateFormatVersion() const override { return core_.stateFormatVersion(); }
  CoreKind kind() const override { return CoreKind::External; }
 private:
  MockCore core_;
  std::shared_ptr<Counts> counts_;
};
struct RegisteredCounter {
  std::shared_ptr<Counts> counts = std::make_shared<Counts>();
  explicit RegisteredCounter(uint32_t flags = CapAudio | CapState | CapSoftReset | CapRecording) {
    auto c = counts;
    registerExternalCore({"counting-rewind-test", "counting-rewind-test/1",
      flags,
      [c] { return std::unique_ptr<ICore>(new CountingCore(c)); }});
  }
  ~RegisteredCounter() { unregisterExternalCore(); }
};
void pictureEvidence(const VideoFrame& a, const VideoFrame& b, const char* label) {
  REQUIRE_EQ(a.width, b.width); REQUIRE_EQ(a.height, b.height);
  uint64_t changed = 0;
  unsigned maxDifference = 0;
  for (uint32_t y = 0; y < a.height; ++y) for (uint32_t x = 0; x < a.width; ++x) {
    const uint32_t av = a.pixels[y * a.stride + x], bv = b.pixels[y * b.stride + x];
    changed += av != bv;
    for (unsigned shift : {0u, 8u, 16u, 24u}) {
      const int d = int((av >> shift) & 255) - int((bv >> shift) & 255);
      maxDifference = std::max(maxDifference, unsigned(d < 0 ? -d : d));
    }
  }
  CHECK_EQ(changed, 0u); CHECK_EQ(maxDifference, 0u);
  const char* dir = std::getenv("RN_REWIND_EVIDENCE");
  if (!dir) return;
  MESSAGE(label << ": changed pixels=" << changed << ", max channel difference=" << maxDifference);
  for (unsigned i = 0; i < 2; ++i) {
    const auto& v = i ? b : a;
    std::ofstream out(std::string(dir) + "/" + label + (i ? "-reference.ppm" : "-cache.ppm"), std::ios::binary);
    REQUIRE(out.good());
    out << "P6\n" << v.width << " " << v.height << "\n255\n";
    for (uint32_t y = 0; y < v.height; ++y) for (uint32_t x = 0; x < v.width; ++x) {
      uint32_t pixel = v.pixels[y * v.stride + x];
      const char rgb[] = {char(pixel >> 16), char(pixel >> 8), char(pixel)};
      out.write(rgb, 3);
    }
    REQUIRE(out.good());
  }
}
void equalVideoCodes(ICore& a, ICore& b) {
  uint32_t aPhase = 0, bPhase = 0;
  uint64_t aFrame = 0, bFrame = 0;
  const uint16_t* ac = a.videoCodes(&aPhase, &aFrame);
  const uint16_t* bc = b.videoCodes(&bPhase, &bFrame);
  REQUIRE(bool(ac) == bool(bc));
  if (!ac) return;
  CHECK_EQ(aPhase, bPhase); CHECK_EQ(aFrame, bFrame);
  CHECK(std::equal(ac, ac + size_t(kVideoWidth) * kVideoHeight, bc));
}
void equalSessions(Session& a, Session& b, ICore& ac, ICore& bc) {
  CHECK_EQ(a.frame(), b.frame());
  CHECK_EQ(a.stateHash(), b.stateHash());
  if (a.videoHash() != b.videoHash())
    MESSAGE("video mismatch core=" << int(a.coreKind()) << ", frame=" << a.frame());
  CHECK_EQ(a.videoHash(), b.videoHash());
  CHECK_EQ(a.audioHash(), b.audioHash());
  CHECK_EQ(ac.audioHash(), bc.audioHash());
  CHECK_EQ(ac.frameIndex(), bc.frameIndex());
  CHECK_EQ(a.videoFrame().width, b.videoFrame().width);
  CHECK_EQ(a.videoFrame().height, b.videoFrame().height);
  CHECK_EQ(a.videoFrame().stride, b.videoFrame().stride);
  equalVideoCodes(ac, bc);
  CHECK_EQ(a.activeTake(), b.activeTake());
  CHECK_EQ(a.takeLength(), b.takeLength());
  CHECK_EQ(a.takes().size(), b.takes().size());
  CHECK_EQ(a.undoDepth(), b.undoDepth());
  CHECK_EQ(a.mode(), b.mode());
  CHECK_EQ(a.practiceBSettable(0), b.practiceBSettable(0));
  CHECK_EQ(a.practiceCounter(), b.practiceCounter());
}
void compareRun(CoreKind kind, uint64_t start, uint64_t stride, bool branch, CheckpointPolicy policy = {}) {
  auto a = newSession(kind, policy), b = newSession(kind, policy);
  REQUIRE(a); REQUIRE(b);
  const auto script = DeterminismHarness::randomScript(127, 1701);
  REQUIRE(recordScript(*a, script).ok());
  REQUIRE(recordScript(*b, script).ok());
  if (branch) {
    REQUIRE(a->seek(49).ok()); REQUIRE(b->seek(49).ok());
    const auto retry = DeterminismHarness::randomScript(78, 1702);
    REQUIRE(recordScript(*a, retry).ok()); REQUIRE(recordScript(*b, retry).ok());
  }
  REQUIRE(a->seek(start).ok()); REQUIRE(b->seek(start).ok());
  REQUIRE(a->practiceSetA(0).ok()); REQUIRE(b->practiceSetA(0).ok());
  ICore& ac = a->core(); ICore& bc = b->core();
  REQUIRE(a->rewind(0).ok()); REQUIRE(b->seek(start).ok());
  equalSessions(*a, *b, ac, bc);
  while (a->frame()) {
    uint64_t target = a->frame() - std::min(stride, a->frame());
    REQUIRE(a->rewind(stride).ok()); REQUIRE(b->seek(target).ok());
    equalSessions(*a, *b, ac, bc);
    if (kind == CoreKind::Nestopia && start == 127 && target == 61)
      pictureEvidence(a->videoFrame(), b->videoFrame(), "rewind-take-61");
  }
  REQUIRE(a->rewind(999).ok()); REQUIRE(b->seek(0).ok());
  equalSessions(*a, *b, ac, bc);
  REQUIRE(a->step(3, 7, 0).ok()); REQUIRE(b->step(3, 7, 0).ok());
  equalSessions(*a, *b, ac, bc);
}
}  // namespace

TEST_CASE("rewind cache: tick targets match seek including boundaries, zero, and new takes") {
  for (CoreKind kind : {CoreKind::Mock, CoreKind::Nestopia, CoreKind::ReplayNEScore}) {
    if (!coreAvailable(kind)) continue;
    for (uint64_t start : {1u, 29u, 30u, 31u, 61u, 127u})
      compareRun(kind, start, start == 127 ? 3 : 1, false);
    compareRun(kind, 127, 2, true);
    compareRun(kind, 127, 4, true);
    compareRun(kind, 127, 3, false, {7, 2, 35});  // evicted dense blocks, nondefault spacing
  }
}

TEST_CASE("rewind cache: one block replay amortizes across display ticks") {
  RegisteredCounter registration;
  for (uint64_t stride : {1u, 2u, 3u, 4u}) {
    auto s = newSession(CoreKind::External);
    REQUIRE(s);
    REQUIRE(recordScript(*s, DeterminismHarness::randomScript(900, 1703)).ok());
    const uint64_t before = registration.counts->steps;
    while (s->frame()) REQUIRE(s->rewind(stride).ok());
    uint64_t emulated = registration.counts->steps - before;
    MESSAGE("stride=" << stride << ", emulated=" << emulated << ", rewound=900");
    CHECK(emulated <= (stride == 1 ? 1900u : 1450u));
  }
}

TEST_CASE("rewind cache: step, seek, mode, and take switches invalidate cached states") {
  RegisteredCounter registration;
  auto s = newSession(CoreKind::External);
  REQUIRE(s);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(127, 1704)).ok());
  const uint64_t originalTake = s->activeTake();
  REQUIRE(s->seek(119).ok());
  REQUIRE(s->rewind(2).ok());
  uint64_t before = registration.counts->steps;
  REQUIRE(s->rewind(2).ok());
  CHECK(registration.counts->steps - before <= 6u);
  REQUIRE(s->step(0x99, 0x42, 0).ok());  // a new take invalidates old input history
  CHECK(s->activeTake() != originalTake);
  before = registration.counts->steps;
  REQUIRE(s->rewind(2).ok());
  CHECK(registration.counts->steps - before > 1);
  REQUIRE(s->seek(s->frame()).ok());
  before = registration.counts->steps;
  REQUIRE(s->rewind(2).ok());
  CHECK(registration.counts->steps - before > 1);
  REQUIRE(s->setMode(Mode::Replay).ok());
  before = registration.counts->steps;
  REQUIRE(s->rewind(2).ok());
  CHECK(registration.counts->steps - before > 1);
  REQUIRE(s->activateTake(originalTake).ok());
  before = registration.counts->steps;
  REQUIRE(s->rewind(2).ok());
  CHECK(registration.counts->steps - before > 1);
}

TEST_CASE("rewind cache: practice matches the legacy snapshot replay and preserves continuity") {
  RegisteredCounter registration;
  for (CoreKind kind : {CoreKind::Mock, CoreKind::Nestopia, CoreKind::ReplayNEScore, CoreKind::External}) {
    if (!coreAvailable(kind)) continue;
    for (uint64_t stride : {1u, 3u, 4u}) {
      auto s = newSession(kind);
      auto reference = createCore(kind == CoreKind::External ? CoreKind::Mock : kind);
      auto rom = buildTestRom();
      REQUIRE(s); REQUIRE(reference);
      REQUIRE(reference->loadROM(rom.data(), rom.size()).ok());
      auto prefix = DeterminismHarness::randomScript(17, 1705);
      REQUIRE(recordScript(*s, prefix).ok());
      for (auto& r : prefix) REQUIRE(reference->stepRecord(r, true).ok());
      REQUIRE(s->setMode(Mode::Practice).ok());
      REQUIRE(s->practiceSetA(0).ok());
      const uint64_t take = s->activeTake();
      std::vector<std::vector<uint8_t>> snapshots(1);
      REQUIRE(reference->saveState(snapshots.back()).ok());
      auto run = DeterminismHarness::randomScript(367, 1706);
      for (size_t i = 0; i < run.size(); ++i) {
        REQUIRE(s->stepInput(run[i]).ok());
        REQUIRE(reference->stepRecord(run[i], true).ok());
        if ((i + 1) % 30 == 0) {
          snapshots.emplace_back();
          REQUIRE(reference->saveState(snapshots.back()).ok());
        }
      }
      ICore& actual = s->core();
      uint64_t current = run.size();
      while (current) {
        uint64_t target = current - std::min(current, stride);
        const auto& snap = snapshots[target / 30];
        REQUIRE(reference->loadState(snap.data(), snap.size()).ok());
        for (uint64_t i = target / 30 * 30; i < target; ++i)
          REQUIRE(reference->stepRecord(run[i], i + 1 == target).ok());
        const uint64_t before = registration.counts->steps;
        REQUIRE(s->rewind(stride).ok());
        if (kind == CoreKind::External && target <= 300)
          CHECK(registration.counts->steps - before <= 6u);
        CHECK_EQ(s->stateHash(), reference->machineHash());
        CHECK_EQ(s->videoHash(), reference->videoHash());
        CHECK_EQ(actual.audioHash(), reference->audioHash());
        equalVideoCodes(actual, *reference);
        if (kind == CoreKind::Nestopia && stride == 1 && target == 61)
          pictureEvidence(s->videoFrame(), reference->videoFrame(), "rewind-practice-61");
        size_t samples = 999;
        s->audio(&samples);
        CHECK_EQ(samples, 0u);
        CHECK_EQ(s->frame(), 17u);
        CHECK_EQ(s->takeLength(), 17u);
        CHECK_EQ(s->activeTake(), take);
        CHECK_EQ(s->practiceCounter(), target);
        CHECK_EQ(s->practiceStatus().rewindAvailable, target);
        CHECK(s->practiceBSettable(0));
        current = target;
      }
      REQUIRE(s->rewind(1000).ok());
      CHECK_EQ(s->practiceCounter(), 0u);
      REQUIRE(s->setMode(Mode::Record).ok());
      CHECK_EQ(s->frame(), 17u);
    }
  }
}

TEST_CASE("rewind cache: practice is amortized and step invalidates the discarded future") {
  RegisteredCounter registration;
  auto s = newSession(CoreKind::External);
  REQUIRE(s);
  REQUIRE(s->setMode(Mode::Practice).ok());
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(900, 1707)).ok());
  const uint64_t before = registration.counts->steps;
  while (s->practiceCounter()) REQUIRE(s->rewind(3).ok());
  const uint64_t emulated = registration.counts->steps - before;
  MESSAGE("practice stride=3, emulated=" << emulated << ", rewound=900");
  CHECK(emulated <= 1450u);
  REQUIRE(recordScript(*s, DeterminismHarness::randomScript(119, 1708)).ok());
  REQUIRE(s->rewind(2).ok());
  uint64_t count = registration.counts->steps;
  REQUIRE(s->rewind(2).ok());
  CHECK(registration.counts->steps - count <= 6u);
  REQUIRE(s->step(17, 93, 0).ok());
  count = registration.counts->steps;
  REQUIRE(s->rewind(2).ok());
  CHECK(registration.counts->steps - count > 1);
}

TEST_CASE("rewind prefetch: several block crossings have bounded work and match seek") {
  RegisteredCounter registration;
  for (uint64_t stride : {2u, 3u, 4u, 0u}) {
    auto actual = newSession(CoreKind::External), reference = newSession(CoreKind::Mock);
    REQUIRE(actual); REQUIRE(reference);
    const auto script = DeterminismHarness::randomScript(900, 1710);
    REQUIRE(recordScript(*actual, script).ok()); REQUIRE(recordScript(*reference, script).ok());
    ICore& ac = actual->core(); ICore& rc = reference->core();
    uint64_t tick = 0, maxWork = 0, prefetchTicks = 0;
    while (actual->frame()) {
      const uint64_t back = stride ? stride : 2 + tick % 3;
      const uint64_t target = actual->frame() - std::min(actual->frame(), back);
      const uint64_t before = registration.counts->steps;
      REQUIRE(actual->rewind(back).ok());
      const uint64_t work = registration.counts->steps - before;
      // The initial miss may build a whole block. Afterwards there is enough
      // time in each 30-frame block to prefetch its predecessor, even at 4/tick.
      if (target <= 840) {
        maxWork = std::max(maxWork, work);
        CHECK(work <= 6u);  // five speculative frames plus the displayed frame
        if (work > 1) ++prefetchTicks;
      }
      REQUIRE(reference->seek(target).ok());
      equalSessions(*actual, *reference, ac, rc);
      ++tick;
    }
    CHECK(prefetchTicks > 0);
    MESSAGE("prefetch stride=" << stride << ", max warm tick=" << maxWork
            << ", prefetch ticks=" << prefetchTicks);
  }
}

TEST_CASE("rewind prefetch: incomplete blocks survive jumps without changing output") {
  RegisteredCounter registration;
  for (CoreKind kind : {CoreKind::External, CoreKind::Nestopia, CoreKind::ReplayNEScore}) {
    if (!coreAvailable(kind)) continue;
    auto actual = newSession(kind), reference = newSession(kind);
    REQUIRE(actual); REQUIRE(reference);
    const auto script = DeterminismHarness::randomScript(607, 1711);
    REQUIRE(recordScript(*actual, script).ok()); REQUIRE(recordScript(*reference, script).ok());
    ICore& ac = actual->core(); ICore& rc = reference->core();
    // The first small ticks cannot finish the next 30-frame block. A jump
    // into it must finish the missing work; a farther jump discards it safely.
    for (uint64_t back : {2u, 3u, 17u, 4u, 71u, 3u, 4u, 2u, 501u}) {
      const uint64_t target = actual->frame() - std::min(actual->frame(), back);
      REQUIRE(actual->rewind(back).ok()); REQUIRE(reference->seek(target).ok());
      equalSessions(*actual, *reference, ac, rc);
    }
  }
}

TEST_CASE("rewind prefetch: step and seek discard pending reconstruction") {
  RegisteredCounter registration;
  for (bool step : {false, true}) {
    auto actual = newSession(CoreKind::External), cold = newSession(CoreKind::External);
    REQUIRE(actual); REQUIRE(cold);
    const auto script = DeterminismHarness::randomScript(600, 1712);
    REQUIRE(recordScript(*actual, script).ok()); REQUIRE(recordScript(*cold, script).ok());
    ICore& ac = actual->core(); ICore& cc = cold->core();
    REQUIRE(actual->rewind(3).ok()); REQUIRE(actual->rewind(3).ok());
    REQUIRE(cold->seek(actual->frame()).ok());
    if (step) {
      REQUIRE(actual->step(0x92, 0x41, 0).ok()); REQUIRE(cold->step(0x92, 0x41, 0).ok());
    } else {
      REQUIRE(actual->seek(actual->frame()).ok()); REQUIRE(cold->seek(cold->frame()).ok());
    }
    // Compare both emulation counts and restore positions to a cold run:
    // merely clearing the completed blocks but resuming old pending work fails.
    for (unsigned tick = 0; tick < 3; ++tick) {
      const uint64_t before = registration.counts->steps;
      registration.counts->loads.clear();
      REQUIRE(actual->rewind(3).ok());
      const uint64_t work = registration.counts->steps - before;
      const auto loads = registration.counts->loads;
      registration.counts->loads.clear();
      const uint64_t referenceBefore = registration.counts->steps;
      REQUIRE(cold->rewind(3).ok());
      CHECK_EQ(work, registration.counts->steps - referenceBefore);
      CHECK(loads == registration.counts->loads);
      equalSessions(*actual, *cold, ac, cc);
    }
  }
}

TEST_CASE("rewind prefetch: autosave sparse replay leaves dense checkpoints for older blocks") {
  RegisteredCounter registration;
  const auto script = DeterminismHarness::randomScript(4316, 1713);
  for (uint64_t stride : {2u, 3u, 4u, 0u}) {
    const std::string root = tempDir("rewind-sparse");
    const std::string romPath = writeTestRom(root);
    const std::string project = fs::join(root, "run.nesrec");
    SessionOptions options;
    options.core = CoreKind::External;
    std::unique_ptr<Session> actual;
    REQUIRE(Session::create(buildTestRom(), romPath, options, actual).ok());
    REQUIRE(actual->saveAs(project).ok());  // only the initial empty project is fully saved
    REQUIRE(recordScript(*actual, script).ok());
    REQUIRE(actual->autosave().ok());
    actual.reset();
    const uint64_t beforeOpen = registration.counts->steps;
    REQUIRE(ProjectStore::open(project, "", options, actual).ok());
    CHECK_EQ(actual->coreKind(), CoreKind::External);
    CHECK_EQ(registration.counts->steps - beforeOpen, 116u);
    REQUIRE_EQ(actual->frame(), 4316u);
    const Checkpoint* seed = actual->checkpoints().best(4199, actual->timeline());
    REQUIRE(seed);
    REQUIRE_EQ(seed->frame, 3600u);
    CHECK_EQ(seed->kind, CpKind::Sparse);
    // Opening only reconstructs 4200..4316. The preceding sparse gap remains cold.
    for (const auto& entry : actual->checkpoints().all())
      if (entry.second.frame > 3600 && entry.second.frame < 4200)
        CHECK(entry.second.kind != CpKind::Dense);

    auto reference = newSession(CoreKind::Mock);
    REQUIRE(reference);
    REQUIRE(recordScript(*reference, script).ok());
    ICore& ac = actual->core(); ICore& rc = reference->core();
    uint64_t tick = 0, firstEntryWork = 0, maxLaterEntry = 0;
    bool crossedSparseGap = false;
    while (actual->frame() > 3960) {
      const uint64_t back = stride ? stride : 2 + tick % 3;
      const uint64_t previous = actual->frame();
      const uint64_t target = previous - back;
      const uint64_t before = registration.counts->steps;
      REQUIRE(actual->rewind(back).ok());
      const uint64_t work = registration.counts->steps - before;
      REQUIRE(reference->seek(target).ok());
      equalSessions(*actual, *reference, ac, rc);
      if (previous > 4200 && target <= 4200) {
        crossedSparseGap = true;
        firstEntryWork = work;  // one replay of the cold sparse gap is allowed
        // Every checkpoint uses the traversed frame's owner, not the visible cursor's.
        auto checkpointCore = createCore(CoreKind::Mock);
        auto checkpointReference = newSession(CoreKind::Mock);
        REQUIRE(checkpointCore); REQUIRE(checkpointReference);
        const auto rom = buildTestRom();
        REQUIRE(checkpointCore->loadROM(rom.data(), rom.size()).ok());
        REQUIRE(recordScript(*checkpointReference, script).ok());
        for (uint64_t frame = 3630; frame < 4200; frame += 30) {
          const auto* cp = actual->checkpoints().find(frame, actual->timeline().stateOwner(frame), int(CpKind::Dense));
          CHECK(cp != nullptr);
          if (!cp) continue;
          REQUIRE(checkpointCore->loadState(cp->data.data(), cp->data.size()).ok());
          REQUIRE(checkpointReference->seek(frame).ok());
          CHECK_EQ(checkpointCore->machineHash(), checkpointReference->stateHash());
        }
      } else if (crossedSparseGap) {
        CHECK(work <= 9u);
        if ((previous - 1) / 30 != (target - 1) / 30) {
          maxLaterEntry = std::max(maxLaterEntry, work);
          MESSAGE("sparse stride=" << stride << ", entry=" << target << ", core frames=" << work);
          CHECK(work <= 30u);
        }
      }
      ++tick;
    }
    REQUIRE(crossedSparseGap);
    MESSAGE("sparse stride=" << stride << ", first entry=" << firstEntryWork
            << ", max later entry=" << maxLaterEntry);
  }
}

TEST_CASE("idle work: bounded scratch warming preserves the current output and stops") {
  RegisteredCounter registration(CapAudio | CapState | CapSoftReset | CapRecording | CapSharesMachine);
  const CheckpointPolicy policy{10, 1200, 120};
  const auto script = DeterminismHarness::randomScript(296, 1714);
  for (CoreKind kind : {CoreKind::Mock, CoreKind::Nestopia, CoreKind::External}) {
    if (!coreAvailable(kind)) continue;
    const std::string root = tempDir("idle-output");
    const std::string romPath = writeTestRom(root);
    const std::string project = fs::join(root, "run.nesrec");
    SessionOptions options;
    options.core = kind; options.checkpoints = policy;
    std::unique_ptr<Session> actual;
    REQUIRE(Session::create(buildTestRom(), romPath, options, actual).ok());
    REQUIRE(actual->saveAs(project).ok());
    REQUIRE(recordScript(*actual, script).ok());
    REQUIRE(actual->autosave().ok());
    actual.reset();
    REQUIRE(ProjectStore::open(project, "", options, actual).ok());
    auto reference = newSession(kind == CoreKind::External ? CoreKind::Mock : kind, policy);
    REQUIRE(reference); REQUIRE(recordScript(*reference, script).ok());
    // Keep valid audible output, not only the already-silent output of an open/seek.
    REQUIRE(actual->step(0x41, 0x83, 0).ok()); REQUIRE(reference->step(0x41, 0x83, 0).ok());
    REQUIRE(actual->practiceSetA(0).ok()); REQUIRE(reference->practiceSetA(0).ok());
    ICore& ac = actual->core(); ICore& rc = reference->core();
    size_t samples = 0; actual->audio(&samples); REQUIRE(samples > 0);
    const bool dirty = actual->unsavedChanges();
    const uint64_t before = registration.counts->steps;
    REQUIRE(actual->idleWork(0).ok());
    CHECK_EQ(registration.counts->steps, before);
    equalSessions(*actual, *reference, ac, rc);
    for (unsigned call = 0; call < 100; ++call) {
      const uint64_t count = registration.counts->steps;
      REQUIRE(actual->idleWork(4).ok());
      if (kind == CoreKind::External) CHECK(registration.counts->steps - count <= 4u);
      equalSessions(*actual, *reference, ac, rc);
      CHECK_EQ(actual->unsavedChanges(), dirty);
    }
    REQUIRE(actual->checkpoints().find(230, actual->timeline().stateOwner(230), int(CpKind::Dense)));
    if (kind == CoreKind::External) CHECK(registration.counts->steps > before);
    const uint64_t completed = registration.counts->steps;
    for (unsigned call = 0; call < 20; ++call) REQUIRE(actual->idleWork(4).ok());
    CHECK_EQ(registration.counts->steps, completed);
    equalSessions(*actual, *reference, ac, rc);
    if (kind == CoreKind::Nestopia)
      pictureEvidence(actual->videoFrame(), reference->videoFrame(), "idle-take-297");
    // Returning to foreground emulation must also preserve the scratch-free reference.
    REQUIRE(actual->step(0x13, 0x72, 0).ok()); REQUIRE(reference->step(0x13, 0x72, 0).ok());
    equalSessions(*actual, *reference, ac, rc);
    REQUIRE(actual->setMode(Mode::Practice).ok()); REQUIRE(reference->setMode(Mode::Practice).ok());
    const uint64_t practicing = registration.counts->steps;
    REQUIRE(actual->idleWork(4).ok());
    CHECK_EQ(registration.counts->steps, practicing);
    equalSessions(*actual, *reference, ac, rc);
  }
}

TEST_CASE("idle work: live sessions are a no-op") {
  RegisteredCounter registration(CapAudio | CapState | CapSoftReset | CapSharesMachine);
  auto s = newSession(CoreKind::External);
  REQUIRE(s); REQUIRE_EQ(s->mode(), Mode::Live);
  REQUIRE(s->step(13, 29, 0).ok());
  const uint64_t count = registration.counts->steps;
  const uint64_t state = s->stateHash(), video = s->videoHash(), audio = s->audioHash();
  REQUIRE(s->idleWork(4).ok());
  CHECK_EQ(registration.counts->steps, count);
  CHECK_EQ(s->stateHash(), state); CHECK_EQ(s->videoHash(), video); CHECK_EQ(s->audioHash(), audio);
}

TEST_CASE("idle work: rewind invalidates a completed idle window") {
  RegisteredCounter registration;
  const std::string root = tempDir("idle-rewind-window");
  const std::string romPath = writeTestRom(root), project = fs::join(root, "run.nesrec");
  SessionOptions options; options.core = CoreKind::External;
  std::unique_ptr<Session> actual;
  REQUIRE(Session::create(buildTestRom(), romPath, options, actual).ok());
  REQUIRE(actual->saveAs(project).ok());
  REQUIRE(recordScript(*actual, DeterminismHarness::randomScript(4316, 1716)).ok());
  REQUIRE(actual->autosave().ok()); actual.reset();
  REQUIRE(ProjectStore::open(project, "", options, actual).ok());
  for (unsigned call = 0; call < 200; ++call) REQUIRE(actual->idleWork(4).ok());
  uint64_t count = registration.counts->steps;
  REQUIRE(actual->idleWork(4).ok()); CHECK_EQ(registration.counts->steps, count);
  REQUIRE(actual->rewind(1500).ok()); REQUIRE_EQ(actual->frame(), 2816u);
  CHECK(!actual->checkpoints().find(2370, actual->timeline().stateOwner(2370), int(CpKind::Dense)));
  count = registration.counts->steps;
  for (unsigned call = 0; call < 200; ++call) REQUIRE(actual->idleWork(4).ok());
  CHECK(registration.counts->steps > count);
  CHECK(actual->checkpoints().find(2370, actual->timeline().stateOwner(2370), int(CpKind::Dense)));
  count = registration.counts->steps;
  REQUIRE(actual->idleWork(4).ok()); CHECK_EQ(registration.counts->steps, count);
}

TEST_CASE("idle work: C API preserves paused output and accepts a zero budget") {
  RegisteredCounter registration(CapAudio | CapState | CapSoftReset | CapRecording | CapSharesMachine);
  const std::string root = tempDir("idle-capi");
  const std::string rom = writeTestRom(root), project = fs::join(root, "run.nesrec");
  rn_session_options options;
  rn_session_options_init(&options);
  options.core = RN_CORE_EXTERNAL; options.dense_interval = 10; options.sparse_interval = 120;
  rn_session* session = nullptr;
  REQUIRE_EQ(rn_session_new(rom.c_str(), project.c_str(), &options, &session), RN_OK);
  for (unsigned i = 0; i < 296; ++i) REQUIRE_EQ(rn_step(session, uint8_t(i), uint8_t(i >> 1), 0, nullptr), RN_OK);
  REQUIRE_EQ(rn_session_autosave(session), RN_OK);
  rn_session_close(session); session = nullptr;
  REQUIRE_EQ(rn_session_open(project.c_str(), nullptr, &options, &session), RN_OK);
  REQUIRE_EQ(rn_step(session, 0x31, 0x91, 0, nullptr), RN_OK);
  const uint64_t state = rn_state_hash(session), video = rn_video_hash(session), audio = rn_audio_hash(session);
  const uint64_t count = registration.counts->steps;
  REQUIRE_EQ(rn_session_idle_work(nullptr, 4), RN_ERR_INVALID_ARG);
  REQUIRE_EQ(rn_session_idle_work(session, 0), RN_OK);
  CHECK_EQ(registration.counts->steps, count);
  for (unsigned call = 0; call < 100; ++call) {
    const uint64_t before = registration.counts->steps;
    REQUIRE_EQ(rn_session_idle_work(session, 4), RN_OK);
    CHECK(registration.counts->steps - before <= 4u);
    CHECK_EQ(rn_frame(session), 297u);
    CHECK_EQ(rn_state_hash(session), state); CHECK_EQ(rn_video_hash(session), video); CHECK_EQ(rn_audio_hash(session), audio);
  }
  CHECK(registration.counts->steps > count);
  const uint64_t complete = registration.counts->steps;
  REQUIRE_EQ(rn_session_idle_work(session, 4), RN_OK);
  CHECK_EQ(registration.counts->steps, complete);
  rn_session_close(session);
}

TEST_CASE("rewind lookahead: idle head start keeps several cold sparse gaps within the tick budget") {
  RegisteredCounter registration(CapAudio | CapState | CapSoftReset | CapRecording | CapSharesMachine);
  const auto script = DeterminismHarness::randomScript(4316, 1715);
  for (uint64_t stride : {2u, 3u, 4u, 0u}) {
    const std::string root = tempDir("lookahead-sparse");
    const std::string romPath = writeTestRom(root), project = fs::join(root, "run.nesrec");
    SessionOptions options; options.core = CoreKind::External;
    std::unique_ptr<Session> actual;
    REQUIRE(Session::create(buildTestRom(), romPath, options, actual).ok());
    REQUIRE(actual->saveAs(project).ok());
    REQUIRE(recordScript(*actual, script).ok()); REQUIRE(actual->autosave().ok());
    actual.reset(); REQUIRE(ProjectStore::open(project, "", options, actual).ok());
    auto reference = newSession(CoreKind::Mock);
    REQUIRE(reference); REQUIRE(recordScript(*reference, script).ok());
    REQUIRE(reference->seek(4315).ok()); REQUIRE(reference->seek(4316).ok());
    ICore& ac = actual->core(); ICore& rc = reference->core();
    for (unsigned call = 0; call < 200; ++call) {
      const uint64_t before = registration.counts->steps;
      REQUIRE(actual->idleWork(4).ok()); CHECK(registration.counts->steps - before <= 4u);
      equalSessions(*actual, *reference, ac, rc);
    }
    // Idle only primes the nearby gap; the next older gaps still need rewind lookahead.
    REQUIRE(actual->checkpoints().find(4170, actual->timeline().stateOwner(4170), int(CpKind::Dense)));
    CHECK(!actual->checkpoints().find(3570, actual->timeline().stateOwner(3570), int(CpKind::Dense)));
    uint64_t tick = 0, maxTick = 0, maxEntry = 0;
    while (actual->frame() > 2816) {
      const uint64_t previous = actual->frame(), back = stride ? stride : 2 + tick % 3;
      const uint64_t target = previous - back, before = registration.counts->steps;
      REQUIRE(actual->rewind(back).ok());
      const uint64_t work = registration.counts->steps - before;
      if (tick) {
        maxTick = std::max(maxTick, work);
        if (work > 9) MESSAGE("lookahead over budget stride=" << stride << ", target=" << target << ", work=" << work);
        CHECK(work <= 9u);
      }
      if ((previous - 1) / 30 != (target - 1) / 30) {
        maxEntry = std::max(maxEntry, work); CHECK(work <= 30u);
      }
      REQUIRE(reference->seek(target).ok()); equalSessions(*actual, *reference, ac, rc);
      ++tick;
    }
    MESSAGE("lookahead stride=" << stride << ", max tick=" << maxTick << ", max entry=" << maxEntry);
  }
}

TEST_CASE("rewind cache: report NES timing without a machine-dependent threshold") {
  if (!std::getenv("RN_REWIND_BENCH")) return;
  for (CoreKind kind : {CoreKind::Mock, CoreKind::Nestopia, CoreKind::ReplayNEScore}) {
    if (!coreAvailable(kind)) continue;
    auto a = newSession(kind), b = newSession(kind);
    REQUIRE(a); REQUIRE(b);
    const auto script = DeterminismHarness::randomScript(600, 1709);
    REQUIRE(recordScript(*a, script).ok()); REQUIRE(recordScript(*b, script).ok());
    const auto start = std::chrono::steady_clock::now();
    while (a->frame()) REQUIRE(a->rewind(3).ok());
    const auto middle = std::chrono::steady_clock::now();
    while (b->frame()) REQUIRE(b->seek(b->frame() - std::min<uint64_t>(3, b->frame())).ok());
    const auto end = std::chrono::steady_clock::now();
    const double cached = std::chrono::duration<double, std::milli>(middle - start).count();
    const double seek = std::chrono::duration<double, std::milli>(end - middle).count();
    MESSAGE("core=" << coreName(kind) << ", 600 frames, stride=3, cache_ms=" << cached
            << ", seek_ms=" << seek << ", speedup=" << seek / cached);
    CHECK_EQ(a->stateHash(), b->stateHash());
  }
}
