#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <memory>
#include <vector>

#include "core/MockCore.h"
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
  std::string compatId() const override { return core_.compatId(); }
  std::string buildId() const override { return "counting-rewind-test"; }
  uint32_t stateFormatVersion() const override { return core_.stateFormatVersion(); }
  CoreKind kind() const override { return CoreKind::External; }
 private:
  MockCore core_;
  std::shared_ptr<Counts> counts_;
};
struct RegisteredCounter {
  std::shared_ptr<Counts> counts = std::make_shared<Counts>();
  RegisteredCounter() {
    auto c = counts;
    registerExternalCore({"counting-rewind-test", MockCore::kCompatId,
      uint32_t(CapAudio | CapState | CapSoftReset | CapRecording),
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
