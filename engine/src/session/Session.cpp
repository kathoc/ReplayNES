#include "session/Session.h"

#include <algorithm>
#include <cstring>

#include "persist/ProjectStore.h"
#include "util/Hash.h"

namespace rn {

Session::~Session() = default;

Status Session::create(std::vector<uint8_t> rom, const std::string& romPath, const SessionOptions& opt,
                       std::unique_ptr<Session>& out) {
  if (opt.checkpoints.denseInterval == 0 || opt.checkpoints.sparseInterval == 0)
    return Error(Err::InvalidArg, "checkpoint intervals must be > 0");
  std::unique_ptr<Session> s(new Session());
  s->opt_ = opt;
  s->cps_ = CheckpointStore(opt.checkpoints);
  if (!coreName(opt.core)) return Error(Err::InvalidArg, "unknown core kind");
  if (!coreAvailable(opt.core)) return Error(Err::UnsupportedFormat, "selected core is not built");
  s->core_ = createCore(opt.core);
  if (!(coreCapabilities(opt.core) & CapRecording)) s->mode_ = Mode::Live;
  s->rom_ = std::move(rom);
  s->romPath_ = romPath;
  s->romSha_ = Sha256::hex(s->rom_.data(), s->rom_.size());
  RN_TRY(s->powerOnFresh());
  if (!s->core_->contentId().empty()) s->romSha_ = s->core_->contentId();
  out = std::move(s);
  return Status::Ok();
}

std::string Session::projectDir() const { return store_ ? store_->dir() : std::string(); }

Status Session::powerOnFresh() {
  RN_TRY(core_->loadContent(romPath_, rom_.data(), rom_.size()));
  layout_ = core_->inputLayout();
  frame_ = 0;
  audioValid_ = false;
  return Status::Ok();
}

namespace {
// The restored picture differs from the one saved before a walk (size included).
bool pictureChanged(const VideoFrame& now, const VideoFrame& before, const std::vector<uint32_t>& saved) {
  return now.width != before.width || now.height != before.height || now.stride != before.stride ||
         std::memcmp(now.pixels, saved.data(), saved.size() * 4) != 0;
}
}  // namespace

uint64_t Session::audioHashOf(const int16_t* a, size_t n, uint32_t channels) { return audioFramesHash(a, n, channels); }

const int16_t* Session::audio(size_t* count) const {
  const int16_t* a = core_->audio(count);
  if (!audioValid_) *count = 0;
  return a;
}

void Session::maybeCheckpoint(uint64_t frame, ICore* source) {
  const CheckpointPolicy& p = cps_.policy();
  bool sparse = frame % p.sparseInterval == 0;
  bool dense = frame % p.denseInterval == 0;
  if (!sparse && !dense) return;
  uint64_t owner = tl_.stateOwner(frame);
  if (sparse) {
    if (cps_.find(frame, owner, int(CpKind::Sparse))) return;
  } else if (cps_.find(frame, owner)) {
    return;
  }
  std::vector<uint8_t> st;
  ICore& machine = source ? *source : *core_;
  if (!machine.saveState(st).ok()) return;  // a missing cache entry only costs seek time
  cps_.add(frame, owner, sparse ? CpKind::Sparse : CpKind::Dense, machine.compatId(), machine.stateFormatVersion(),
           std::move(st));
}

Status Session::step(uint8_t p1, uint8_t p2, uint8_t events, StepInfo* info) {
  return stepInput(InputRecord{p1, p2, events}, info);
}

Status Session::stepInput(const InputRecord& in, StepInfo* info) {
  clearRewindCache();
  const uint8_t events = in.events;
  if (mode_ != Mode::Replay)
    for (size_t b = layout_.recordBytes; b < kInputRecordBytes; ++b)
      if (in.byte(b)) return Error(Err::InvalidArg, "input uses bytes the core's input layout does not have");
  StepInfo si;
  si.mode = mode_;
  if (mode_ == Mode::Live) {
    if (events & ~kEvMask) return Error(Err::InvalidArg, "unknown event flags");
    if ((events & kEvSoftReset) && !(events & kEvPowerCycle))
      return Error(Err::UnsupportedFormat, "soft reset is unavailable for this live core");
    RN_TRY(core_->stepRecord(in, true));
    ++frame_;
    audioValid_ = true;
    si.frame = frame_;
    si.applied = in;
    if (info) *info = si;
    return Status::Ok();
  }
  InputRecord r;
  if (mode_ == Mode::Practice) {
    if (events & ~kEvMask) return Error(Err::InvalidArg, "unknown event flags");
    r = in;
    RN_TRY(core_->stepRecord(r, true));
    // Nothing is recorded: no timeline, no take checkpoints, no change counter.
    pInputs_.push_back(r);
    ++emuCount_;
    audioValid_ = true;
    const uint64_t interval = cps_.policy().denseInterval;
    if ((emuCount_ - pSnaps_.front().count) % interval == 0) {
      PracticeSnap snap{emuCount_, {}};
      if (captureState(snap.state).ok()) {  // a missing snapshot only shortens rewind
        pSnaps_.push_back(std::move(snap));
        const size_t cap = std::max<size_t>(2, cps_.policy().denseCapacity);
        while (pSnaps_.size() > cap) {
          uint64_t dropTo = pSnaps_[1].count;
          pInputs_.erase(pInputs_.begin(), pInputs_.begin() + ptrdiff_t(dropTo - pSnaps_.front().count));
          pSnaps_.pop_front();
        }
      }
    }
    si.frame = frame_;
    si.applied = r;
    si.take = tl_.head();
    if (info) *info = si;
    return Status::Ok();
  }
  if (mode_ == Mode::Replay) {
    const InputRecord* rec = tl_.recordAt(frame_);
    if (!rec) {
      si.frame = frame_;
      si.endOfTake = true;
      si.take = tl_.head();
      if (info) *info = si;
      return Status::Ok();
    }
    r = *rec;
  } else {
    if (events & ~kEvMask) return Error(Err::InvalidArg, "unknown event flags");
    r = in;
  }
  // Emulate first; only a frame that was really emulated gets recorded.
  RN_TRY(core_->stepRecord(r, true));
  if (mode_ == Mode::Record) {
    bool branched = false;
    Status st = tl_.record(frame_, r, branched);
    if (!st.ok()) return Error(Err::Internal, "timeline rejected an emulated frame: " + st.message);
    si.branched = branched;
    touch();
  }
  ++frame_;
  ++emuCount_;
  audioValid_ = true;
  maybeCheckpoint(frame_);
  si.frame = frame_;
  si.applied = r;
  si.take = tl_.head();
  si.endOfTake = mode_ == Mode::Replay && frame_ >= tl_.length();
  if (info) *info = si;
  return Status::Ok();
}

void Session::clearRewindCache() {
  rewindBlocks_.clear();
  rewindPrefetch_ = {};
  warmCore_.reset();
  warmNext_ = warmEnd_ = 0;
  idleWarmComplete_ = false;
  warmChecked_ = false;
}

uint64_t Session::advanceRewindPrefetch(uint64_t budget) {
  uint64_t used = 0;
  auto& work = rewindPrefetch_;
  if (!work.end) return 0;
  // This walk must not use resync/step: it changes neither cursor, continuity
  // nor the practice input ring. The caller always restores the
  // requested pre-frame state and renders the result afterwards, even on error.
  Status st = work.fresh ? core_->loadContent(romPath_, rom_.data(), rom_.size())
                         : core_->loadState(work.resume.data(), work.resume.size());
  work.fresh = false;
  while (st.ok() && budget && work.next < work.end) {
    if (work.next >= work.block.first) {
      std::vector<uint8_t> state;
      st = captureState(state);
      if (!st.ok()) break;
      work.block.states.push_back(std::move(state));
    }
    const InputRecord* input = mode_ == Mode::Practice
        ? &pInputs_[size_t(work.next - pSnaps_.front().count)] : tl_.recordAt(work.next);
    if (!input) { st = Error(Err::Internal, "missing prefetch input"); break; }
    st = core_->stepRecord(*input, false);
    ++used;
    if (st.ok()) {
      ++work.next;
      // A cold sparse gap can span many dense blocks. Keep the checkpoints
      // traversed here just as resync does, so the next block does not replay
      // that same gap again. Use the walk's position, never the visible cursor.
      if (mode_ != Mode::Practice) maybeCheckpoint(work.next);
    }
    --budget;
  }
  if (st.ok() && work.next == work.end) {
    retainRewindBlock(std::move(work.block));
    work = {};
  } else if (!st.ok() || !captureState(work.resume).ok()) {
    // A speculative cache failure must not turn an otherwise valid rewind
    // into an error. The ordinary restore/replay path remains authoritative.
    work = {};
  }
  return used;
}

uint64_t Session::finishRewindPrefetch(uint64_t target) {
  const auto& work = rewindPrefetch_;
  if (target > 0 && work.end && target - 1 >= work.block.first && target - 1 < work.end &&
      !rewindState(target - 1))
    return advanceRewindPrefetch(work.end - work.next);
  return 0;
}

uint64_t Session::prefetchOlderRewindBlock(uint64_t target, uint64_t budget, uint64_t rewound) {
  if (!budget) return 0;
  const uint64_t interval = cps_.policy().denseInterval;
  uint64_t first, end, seed;
  const std::vector<uint8_t>* initial = nullptr;
  if (mode_ == Mode::Practice) {
    size_t k = pSnaps_.size();
    while (k && pSnaps_[k - 1].count > target - 1) --k;
    if (k < 2) return 0;
    end = pSnaps_[k - 1].count;
    const auto& snap = pSnaps_[k - 2];
    seed = snap.count;
    first = end - std::min(end - seed, interval);
    if (end <= practiceFloor()) return 0;
    initial = &snap.state;
  } else {
    end = (target - 1) / interval * interval;
    if (!end) return 0;
    first = end - interval;
    const auto* cp = cps_.best(first, tl_);
    seed = cp ? cp->frame : 0;
    if (seed) initial = &cp->data;
    // Take frame zero is always fresh power-on, never a serialized state.
    first = std::max<uint64_t>(1, first);
  }
  if (first >= end || (rewindState(first) && rewindState(end - 1))) return 0;
  auto& work = rewindPrefetch_;
  if (work.end != end || work.block.first != first) {
    work = {};
    work.block.first = first;
    work.next = seed;
    work.end = end;
    work.fresh = !initial;
    if (initial) work.resume = *initial;
  }
  if (rewound) {
    // Include this tick, then only ticks strictly before entering the older
    // block. Spread the remaining work over those opportunities instead of
    // front-loading five frames regardless of the transport's actual speed.
    const uint64_t ticks = 1 + (target - end - 1) / rewound;
    const uint64_t left = end - work.next;
    budget = std::min(budget, left / ticks + (left % ticks != 0));
  }
  return advanceRewindPrefetch(budget);
}

Status Session::warmRewindGap(uint64_t target, uint32_t budgetFrames, bool idle) {
  if (!budgetFrames || !target) return Status::Ok();
  const uint64_t dense = cps_.policy().denseInterval;
  const uint64_t sparse = cps_.policy().sparseInterval;
  // The dense seed is needed when building the adjacent block, one block
  // BEFORE rewind enters the gap. Allow a second block for uneven 2/3/4 ticks
  // and their near-cache priority, without warming a whole extra sparse gap.
  const uint64_t lead = 2 * dense;
  const uint64_t planningFrame = target - 1 > lead ? target - 1 - lead : 0;
  const uint64_t region = planningFrame / sparse * sparse;
  // Rewind needs the immediately older sparse region, not a rolling two-gap
  // horizon. The stable boundary also avoids rescanning a warm window each tick.
  const uint64_t floor = idle ? (target > sparse ? target - sparse : 0)
                              : (region >= sparse ? region - sparse : 0);
  // A jump or another reconstruction may have made the pending walk obsolete.
  if (warmEnd_ && (warmEnd_ >= target || warmEnd_ <= floor ||
      cps_.find(warmEnd_, tl_.stateOwner(warmEnd_)))) warmEnd_ = 0;
  if (!warmEnd_) {
    if (warmChecked_ && warmCheckedId_ == cps_.nextId() &&
        floor >= warmCheckedFloor_ && target - 1 <= warmCheckedTop_) {
      if (idle) idleWarmComplete_ = true;
      return Status::Ok();
    }
    uint64_t end = (target - 1) / dense * dense;
    bool usedBlockCache = false;
    while (end > floor) {
      if (!cps_.find(end, tl_.stateOwner(end))) {
        if (!rewindState(end)) break;
        usedBlockCache = true;
      }
      end -= dense;
    }
    if (end <= floor) {
      // A block cache is transient; only checkpoint-backed coverage is memoized.
      if (!usedBlockCache) {
        warmChecked_ = true;
        warmCheckedFloor_ = floor;
        warmCheckedTop_ = target - 1;
        warmCheckedId_ = cps_.nextId();
      }
      if (idle) idleWarmComplete_ = true;
      return Status::Ok();
    }
    const auto* cp = cps_.best(end, tl_);
    const uint64_t seed = cp ? cp->frame : 0;
    if (!warmCore_) {
      warmCore_ = createCore(core_->kind());
      if (!warmCore_) return Error(Err::UnsupportedFormat, "no core for idle reconstruction");
      Status loaded = warmCore_->loadContent(romPath_, rom_.data(), rom_.size());
      if (!loaded.ok()) { warmCore_.reset(); return loaded; }
    } else if (!seed) {
      RN_TRY(warmCore_->loadContent(romPath_, rom_.data(), rom_.size()));
    }
    if (seed) RN_TRY(warmCore_->loadState(cp->data.data(), cp->data.size()));
    warmNext_ = seed;
    warmEnd_ = end;
  }
  for (uint32_t i = 0; i < budgetFrames && warmNext_ < warmEnd_; ++i) {
    const auto* input = tl_.recordAt(warmNext_);
    if (!input) { warmEnd_ = 0; return Error(Err::Internal, "missing warm-up input"); }
    Status step = warmCore_->stepRecord(*input, false);
    if (!step.ok()) { warmEnd_ = 0; return step; }
    ++warmNext_;
    maybeCheckpoint(warmNext_, warmCore_.get());
  }
  if (warmNext_ == warmEnd_) warmEnd_ = 0;
  return Status::Ok();
}

Status Session::idleWork(uint32_t budgetFrames) {
  if (!budgetFrames || idleWarmComplete_ || mode_ == Mode::Practice || mode_ == Mode::Live)
    return Status::Ok();
  // Use another logical instance: loading a saved state into the foreground
  // core would clear audio or change video on several supported cores. Keeping
  // it untouched also preserves frame-zero and load-only presentation exactly.
  return warmRewindGap(frame_, budgetFrames, true);
}

const std::vector<uint8_t>* Session::rewindState(uint64_t frame) const {
  for (const auto& block : rewindBlocks_)
    if (frame >= block.first && frame - block.first < block.states.size())
      return &block.states[size_t(frame - block.first)];
  return nullptr;
}

void Session::cacheRewindState(RewindBlock& block, uint64_t frame) {
  if (frame < block.first) return;
  std::vector<uint8_t> state;
  if (!captureState(state).ok()) {
    // Like checkpoints, this is only an acceleration cache. A failed capture
    // must not make a previously valid seek fail or leave a hole in the range.
    block.states.clear();
    block.first = frame + 1;
    return;
  }
  if (block.states.empty()) block.first = frame;
  block.states.push_back(std::move(state));
  // Practice snapshots may be farther apart after a failed snapshot capture.
  if (block.states.size() > cps_.policy().denseInterval) {
    block.states.erase(block.states.begin());
    ++block.first;
  }
}

void Session::retainRewindBlock(RewindBlock block) {
  if (block.states.empty()) return;
  if (rewindBlocks_.size() == 2) rewindBlocks_.pop_front();
  rewindBlocks_.push_back(std::move(block));
}

Status Session::resync(uint64_t target, bool forceReload, RewindBlock* block) {
  if (target > tl_.length()) return Error(Err::OutOfRange, "seek target beyond take end");
  if (!forceReload && target == frame_) return Status::Ok();
  ++emuEpoch_;  // any seek / reload breaks emulation continuity (A/B anchors)
  const Checkpoint* cp = target > 0 ? cps_.best(target - 1, tl_) : nullptr;
  bool continueCurrent = !forceReload && target > frame_ && (!cp || cp->frame <= frame_);
  if (!continueCurrent) {
    if (cp && cp->frame > 0) {
      Status st = core_->loadState(cp->data.data(), cp->data.size());
      if (!st.ok()) {
        // Never silently fall back: report the broken cache entry.
        return Error(st.code, "checkpoint " + std::to_string(cp->id) + " failed to load: " + st.message);
      }
      frame_ = cp->frame;
    } else {
      RN_TRY(powerOnFresh());  // frame 0 = fresh power-on (bit-identical to a new instance)
    }
  }
  while (frame_ < target) {
    const InputRecord* r = tl_.recordAt(frame_);
    if (!r) return Error(Err::Internal, "missing record during seek");
    // Take frame zero is a fresh content load, not a serialized checkpoint.
    // In particular, restoring Nestopia's power-on state does not reset its
    // picture buffer, and the first frame need not overwrite that buffer.
    if (block && frame_ > 0) cacheRewindState(*block, frame_);
    RN_TRY(core_->stepRecord(*r, frame_ + 1 == target));
    ++frame_;
    maybeCheckpoint(frame_);
  }
  audioValid_ = false;  // seeking is silent
  return Status::Ok();
}

Status Session::seek(uint64_t target) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (mode_ == Mode::Practice) return Error(Err::WrongMode, "cannot seek the take while practicing (leave practice first)");
  RN_TRY(resync(target, false));
  touch();
  return Status::Ok();
}

Status Session::rewind(uint64_t n) {
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (mode_ == Mode::Practice) return practiceRewind(n);
  const uint64_t target = frame_ - std::min(n, frame_);
  if (target != frame_) {
    idleWarmComplete_ = false;
    if (target > 0) {
      const uint64_t finished = finishRewindPrefetch(target);
      if (const auto* state = rewindState(target - 1)) {
        // Prefetch may evict a block. Own the state that restores this tick,
        // then render its final frame AFTER all speculative core work.
        const auto saved = *state;
        const uint64_t consumed = frame_ - target;
        const uint64_t pace = std::min<uint64_t>(4, consumed);
        const uint64_t budget = 2 * pace;
        const uint64_t remaining = finished < budget ? budget - finished : 0;
        const uint64_t nearRemaining = finished < 5 ? 5 - finished : 0;
        const uint64_t used = prefetchOlderRewindBlock(
            target, std::min({pace + 1, remaining, nearRemaining}), consumed);
        // Never spend unused near-block budget on extra distant work. A cold
        // region is walked at the rate rewind consumes it, and only in reach.
        (void)warmRewindGap(target, uint32_t(std::min(pace, remaining - used)), false);
        ++emuEpoch_;  // exactly the discontinuity that resync would report
        RN_TRY(core_->loadState(saved.data(), saved.size()));
        frame_ = target - 1;
        const InputRecord* r = tl_.recordAt(frame_);
        if (!r) return Error(Err::Internal, "missing record during rewind");
        RN_TRY(core_->stepRecord(*r, true));
        ++frame_;
        maybeCheckpoint(frame_);
        audioValid_ = false;
      } else {
        if (rewindBlocks_.size() == 2) rewindBlocks_.pop_front();
        RewindBlock block;
        const uint64_t interval = cps_.policy().denseInterval;
        block.first = (target - 1) / interval * interval;
        RN_TRY(resync(target, false, &block));
        retainRewindBlock(std::move(block));
      }
    } else {
      RN_TRY(resync(0, false));  // fresh power-on, including the initial picture
    }
  }
  touch();  // seek touches even when the cursor does not move
  return Status::Ok();
}

// ------------------------------------------------------------------ bookmarks
Status Session::bookmarkAdd(const std::string& name, uint64_t* id) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (mode_ == Mode::Practice) return Error(Err::WrongMode, "cannot add a take bookmark while practicing");
  Bookmark b;
  b.id = nextBookmarkId_++;
  b.name = name;
  b.frame = frame_;
  b.owner = tl_.stateOwner(frame_);
  if (frame_ > 0) {
    std::vector<uint8_t> st;
    RN_TRY(captureState(st));
    b.stateId = cps_.add(frame_, b.owner, CpKind::Bookmark, core_->compatId(), core_->stateFormatVersion(), std::move(st));
  }
  bookmarks_.push_back(b);
  if (id) *id = b.id;
  touch();
  return Status::Ok();
}

static Bookmark* findBm(std::vector<Bookmark>& v, uint64_t id) {
  for (auto& b : v) if (b.id == id) return &b;
  return nullptr;
}

Status Session::bookmarkRemove(uint64_t id) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  for (size_t i = 0; i < bookmarks_.size(); ++i) {
    if (bookmarks_[i].id != id) continue;
    if (bookmarks_[i].stateId) cps_.remove(bookmarks_[i].stateId);
    bookmarks_.erase(bookmarks_.begin() + ptrdiff_t(i));
    touch();
    return Status::Ok();
  }
  return Error(Err::NotFound, "bookmark not found");
}

Status Session::bookmarkRename(uint64_t id, const std::string& name) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  Bookmark* b = findBm(bookmarks_, id);
  if (!b) return Error(Err::NotFound, "bookmark not found");
  b->name = name;
  touch();
  return Status::Ok();
}

Status Session::bookmarkGoto(uint64_t id) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (mode_ == Mode::Practice) return Error(Err::WrongMode, "cannot jump on the take while practicing");
  Bookmark* b = findBm(bookmarks_, id);
  if (!b) return Error(Err::NotFound, "bookmark not found");
  if (!tl_.stateValid(b->frame, b->owner)) {
    // The bookmark lives on another take: switch to the take ending at its owner segment.
    if (!tl_.segment(b->owner)) return Error(Err::Corrupt, "bookmark refers to a missing take");
    tl_.undo().push_back({tl_.head(), frame_});
    RN_TRY(tl_.setHead(b->owner));
    Status st = resync(b->frame, true);
    touch();
    return st;
  }
  return seek(b->frame);
}

// ------------------------------------------------------------------ takes
std::vector<TakeInfo> Session::takes() const {
  std::vector<TakeInfo> out;
  if (mode_ == Mode::Live) return out;
  for (auto& kv : tl_.segments()) {
    const Segment& s = kv.second;
    out.push_back({s.id, s.parent, s.start, s.end(), s.createdSeq, s.id == tl_.head(), tl_.childCount(s.id)});
  }
  return out;
}

Status Session::activateTake(uint64_t id) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (mode_ == Mode::Practice) return Error(Err::WrongMode, "cannot switch takes while practicing");
  if (!tl_.segment(id)) return Error(Err::NotFound, "take not found");
  if (id == tl_.head()) return Status::Ok();
  tl_.undo().push_back({tl_.head(), frame_});
  RN_TRY(tl_.setHead(id));
  uint64_t target = frame_ < tl_.length() ? frame_ : tl_.length();
  touch();
  return resync(target, true);
}

Status Session::undoTakeSwitch() {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (mode_ == Mode::Practice) return Error(Err::WrongMode, "cannot switch takes while practicing");
  if (tl_.undo().empty()) return Error(Err::NotFound, "nothing to undo");
  UndoEntry e = tl_.undo().back();
  tl_.undo().pop_back();
  RN_TRY(tl_.setHead(e.head));
  uint64_t target = e.frame < tl_.length() ? e.frame : tl_.length();
  touch();
  return resync(target, true);
}

static bool takeReferenced(const std::vector<Bookmark>& bms, const std::array<PracticeSlot, kPracticeSlots>& slots,
                           uint64_t id) {
  for (const Bookmark& b : bms)
    if (b.owner == id) return true;
  for (const PracticeSlot& p : slots)
    if (p.used && p.hasTakeFrame && p.takeId == id) return true;
  return false;
}

Status Session::discardTake(uint64_t id) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (!tl_.segment(id)) return Error(Err::NotFound, "take not found");
  if (takeReferenced(bookmarks_, slots_, id))
    return Error(Err::InvalidArg, "a bookmark or an A/B section is on take " + std::to_string(id));
  RN_TRY(tl_.removeLeaf(id));
  std::vector<uint64_t> drop;
  for (auto& kv : cps_.all())
    if (kv.second.owner == id) drop.push_back(kv.first);
  for (uint64_t c : drop) cps_.remove(c);
  touch();
  return Status::Ok();
}

Status Session::truncateTake() {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (mode_ == Mode::Practice) return Error(Err::WrongMode, "cannot cut the take while practicing");
  if (frame_ >= tl_.length()) return Status::Ok();
  const uint64_t oldId = tl_.head();
  if (takeReferenced(bookmarks_, slots_, oldId))
    return Error(Err::InvalidArg, "a bookmark or an A/B section is on the active take");
  uint64_t newId = 0;
  RN_TRY(tl_.truncateHead(frame_, newId));
  // States of the kept frames are the same machine states on the copy: re-own them (as new,
  // unpersisted checkpoints - a persisted state file names its owner); the rest go.
  std::vector<uint64_t> drop;
  std::vector<Checkpoint> keep;
  for (auto& kv : cps_.all()) {
    const Checkpoint& c = kv.second;
    if (c.owner != oldId) continue;
    drop.push_back(kv.first);
    if (c.frame <= frame_ && (c.kind == CpKind::Dense || c.kind == CpKind::Sparse)) keep.push_back(c);
  }
  for (uint64_t c : drop) cps_.remove(c);
  for (Checkpoint& c : keep) cps_.add(c.frame, newId, c.kind, c.compat, c.stateFormatVersion, std::move(c.data));
  touch();
  return Status::Ok();
}

// ------------------------------------------------------------------ practice
Status Session::setMode(Mode m) {
  clearRewindCache();
  if (mode_ == Mode::Live || m == Mode::Live) {
    if (mode_ == m) return Status::Ok();
    return Error(Err::UnsupportedFormat, "live execution cannot change to a recording mode");
  }
  if (m == mode_) return Status::Ok();
  if (m == Mode::Practice) return enterPractice();
  if (mode_ == Mode::Practice) return leavePractice(m);
  mode_ = m;
  touch();
  return Status::Ok();
}

Status Session::cursorState(std::vector<uint8_t>& out) {
  if (mode_ == Mode::Practice) {
    out = returnState_;
    return Status::Ok();
  }
  return captureState(out);
}

void Session::resetPracticeRun(std::vector<uint8_t> base) {
  pSnaps_.clear();
  pInputs_.clear();
  pSnaps_.push_back({emuCount_, std::move(base)});
}

Status Session::enterPractice() {
  std::vector<uint8_t> st;
  RN_TRY(captureState(st));
  returnMode_ = mode_;
  returnState_ = st;
  mode_ = Mode::Practice;
  // Continuous with the take: anchors set before entering stay live.
  pEntryEpoch_ = emuEpoch_;
  pEntryCount_ = emuCount_;
  curAnchorSlot_ = -1;
  curAnchorEpoch_ = emuEpoch_;
  curAnchorCount_ = emuCount_;
  resetPracticeRun(std::move(st));
  return Status::Ok();  // the persisted mode/cursor are unchanged: not a project change
}

Status Session::leavePractice(Mode to) {
  Status st = frame_ == 0 ? powerOnFresh() : core_->loadState(returnState_.data(), returnState_.size());
  if (!st.ok()) {
    mode_ = returnMode_;  // fall back to rebuilding the take state from the input log
    uint64_t f = frame_;
    st = resync(f, true);
    if (!st.ok()) return st;
  }
  ++emuEpoch_;
  audioValid_ = false;
  pSnaps_.clear();
  pInputs_.clear();
  returnState_.clear();
  returnState_.shrink_to_fit();
  curAnchorSlot_ = -1;
  mode_ = to;
  if (to != returnMode_) touch();
  return Status::Ok();
}

uint64_t Session::practiceFloor() const {
  uint64_t floor = pSnaps_.empty() ? emuCount_ : pSnaps_.front().count;
  if (curAnchorEpoch_ == emuEpoch_ && curAnchorCount_ > floor) floor = curAnchorCount_;
  return floor > emuCount_ ? emuCount_ : floor;
}

Status Session::practiceRewind(uint64_t n) {
  uint64_t floor = practiceFloor();
  uint64_t back = std::min<uint64_t>(n, emuCount_ - floor);
  if (back == 0) return Status::Ok();
  uint64_t target = emuCount_ - back;
  size_t k = pSnaps_.size();
  while (k > 0 && pSnaps_[k - 1].count > target) --k;
  if (k == 0) return Error(Err::Internal, "practice rewind: no snapshot");
  const PracticeSnap& snap = pSnaps_[k - 1];
  const uint64_t inputBase = pSnaps_.front().count;
  // At an exact snapshot boundary the old path only loads the snapshot. Some
  // cores leave the video buffer untouched on load; preserve that behavior.
  if (target > snap.count) finishRewindPrefetch(target);
  const auto* cached = target > snap.count ? rewindState(target - 1) : nullptr;
  if (cached) {
    const auto saved = *cached;
    prefetchOlderRewindBlock(target);
    RN_TRY(core_->loadState(saved.data(), saved.size()));
    RN_TRY(core_->stepRecord(pInputs_[size_t(target - 1 - inputBase)], true));
  } else {
    if (rewindBlocks_.size() == 2) rewindBlocks_.pop_front();
    RewindBlock block;
    block.first = target - std::min<uint64_t>(target - snap.count, cps_.policy().denseInterval);
    RN_TRY(core_->loadState(snap.state.data(), snap.state.size()));
    for (uint64_t c = snap.count; c < target; ++c) {
      cacheRewindState(block, c);
      const InputRecord& r = pInputs_[size_t(c - inputBase)];
      RN_TRY(core_->stepRecord(r, c + 1 == target));
    }
    retainRewindBlock(std::move(block));
  }
  pSnaps_.erase(pSnaps_.begin() + ptrdiff_t(k), pSnaps_.end());
  pInputs_.resize(size_t(target - inputBase));
  emuCount_ = target;  // same epoch: the run from the anchor is still unbroken
  for (auto& p : slots_)
    if (p.anchorEpoch == emuEpoch_ && p.anchorCount > target) p.anchorEpoch = 0;  // A lies in the discarded future
  audioValid_ = false;
  return Status::Ok();
}

bool Session::checkSlot(int slot, Status& err) const {
  if (slot < 0 || slot >= kPracticeSlots) {
    err = Error(Err::OutOfRange, "practice slot must be 0.." + std::to_string(kPracticeSlots - 1));
    return false;
  }
  return true;
}

const PracticeSlot* Session::practiceSlot(int slot) const {
  return slot >= 0 && slot < kPracticeSlots ? &slots_[size_t(slot)] : nullptr;
}

Status Session::practiceSetA(int slot) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  Status err;
  if (!checkSlot(slot, err)) return err;
  std::vector<uint8_t> st;
  RN_TRY(captureState(st));
  PracticeSlot& p = slots_[size_t(slot)];
  if (!p.used) {
    p = PracticeSlot();
    p.used = true;
    p.createdSeq = ++practiceSeq_;
    p.updatedSeq = p.createdSeq;
  } else {
    p.updatedSeq = ++practiceSeq_;
  }
  p.stateSeq = p.updatedSeq;
  p.crc = crc32(st.data(), st.size());
  p.state = std::move(st);
  p.compat = core_->compatId();
  p.stateFormatVersion = core_->stateFormatVersion();
  p.hasB = false;
  p.length = 0;
  // On the take (or at the untouched practice entry point, which is the same machine state).
  bool onTake = mode_ != Mode::Practice || (emuEpoch_ == pEntryEpoch_ && emuCount_ == pEntryCount_);
  p.hasTakeFrame = onTake;
  p.takeFrame = onTake ? frame_ : 0;
  p.takeId = onTake ? tl_.head() : 0;
  p.anchorEpoch = emuEpoch_;
  p.anchorCount = emuCount_;
  p.stateJournaled = p.stateFiled = false;
  curAnchorSlot_ = slot;
  curAnchorEpoch_ = emuEpoch_;
  curAnchorCount_ = emuCount_;
  touch();
  return Status::Ok();
}

bool Session::practiceBSettable(int slot) const {
  const PracticeSlot* p = practiceSlot(slot);
  return p && p->used && p->anchorEpoch != 0 && p->anchorEpoch == emuEpoch_ && emuCount_ >= p->anchorCount;
}

Status Session::practiceSetB(int slot) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  Status err;
  if (!checkSlot(slot, err)) return err;
  PracticeSlot& p = slots_[size_t(slot)];
  if (!p.used) return Error(Err::NotFound, "practice slot " + std::to_string(slot) + " has no A");
  if (!practiceBSettable(slot))
    return Error(Err::Discontinuity, "emulation was not continuous since A of slot " + std::to_string(slot) +
                                         " (seek/load/take switch in between): set A again or go to A first");
  uint64_t len = emuCount_ - p.anchorCount;
  if (len == 0) return Error(Err::InvalidArg, "B must be at least one frame after A");
  p.hasB = true;
  p.length = len;
  p.updatedSeq = ++practiceSeq_;
  touch();
  return Status::Ok();
}

Status Session::takeStateAt(uint64_t target, std::vector<uint8_t>& out) {
  const Checkpoint* cp = target > 0 ? cps_.best(target - 1, tl_) : nullptr;
  bool continueCurrent = target >= frame_ && (!cp || cp->frame <= frame_);
  if (!continueCurrent) {
    if (cp && cp->frame > 0) {
      Status st = core_->loadState(cp->data.data(), cp->data.size());
      if (!st.ok()) return Error(st.code, "checkpoint " + std::to_string(cp->id) + " failed to load: " + st.message);
      frame_ = cp->frame;
    } else {
      RN_TRY(core_->loadContent(romPath_, rom_.data(), rom_.size()));  // fresh power-on (video buffer untouched below)
      frame_ = 0;
    }
  }
  while (frame_ < target) {
    const InputRecord* r = tl_.recordAt(frame_);
    if (!r) return Error(Err::Internal, "missing record while locating A");
    RN_TRY(core_->stepRecord(*r, false));
    ++frame_;
    maybeCheckpoint(frame_);
  }
  return captureState(out);
}

Status Session::practiceSetRange(int slot, uint64_t a, uint64_t b) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  Status err;
  if (!checkSlot(slot, err)) return err;
  if (mode_ == Mode::Practice)
    return Error(Err::WrongMode, "cannot define a range on the take while practicing (leave practice first)");
  if (a >= b) return Error(Err::InvalidArg, "A must be before B");
  if (b > tl_.length()) return Error(Err::OutOfRange, "B is beyond the take end");
  std::vector<uint8_t> stA;
  if (a == frame_) {
    RN_TRY(captureState(stA));
  } else {
    // Walk to A on the take, then put the cursor state back exactly (state, frame, video,
    // continuity epoch). Only the audio of the last frame is lost (reported as 0 samples).
    std::vector<uint8_t> saved;
    const VideoFrame vf = core_->videoFrame();
    std::vector<uint32_t> savedVideo(vf.pixels, vf.pixels + size_t(vf.stride) * vf.height);
    RN_TRY(captureState(saved));
    const uint64_t savedFrame = frame_, savedEpoch = emuEpoch_;
    Status walk = takeStateAt(a, stA);
    // Frame 0 is restored like everywhere else: a fresh power-on (bit-identical to a new instance).
    Status back = savedFrame == 0 ? powerOnFresh() : core_->loadState(saved.data(), saved.size());
    frame_ = savedFrame;
    audioValid_ = false;
    bool rebuild = !back.ok() ||  // never leave the session on a wrong state
                   pictureChanged(core_->videoFrame(), vf, savedVideo);  // picture cleared
    if (rebuild) {
      // Rebuild the cursor (state + picture) from the input log. The machine state equals the
      // one before the call, so emulation continuity (A/B anchors) is kept.
      Status rs = resync(savedFrame, true);
      if (!rs.ok()) return rs;
      audioValid_ = false;
    }
    emuEpoch_ = savedEpoch;
    if (!walk.ok()) return walk;
  }
  PracticeSlot& p = slots_[size_t(slot)];
  if (!p.used) {
    p = PracticeSlot();
    p.used = true;
    p.createdSeq = ++practiceSeq_;
    p.updatedSeq = p.createdSeq;
  } else {
    p.updatedSeq = ++practiceSeq_;
  }
  p.stateSeq = p.updatedSeq;
  p.crc = crc32(stA.data(), stA.size());
  p.state = std::move(stA);
  p.compat = core_->compatId();
  p.stateFormatVersion = core_->stateFormatVersion();
  p.hasB = true;
  p.length = b - a;
  p.hasTakeFrame = true;
  p.takeFrame = a;
  p.takeId = tl_.head();
  p.anchorEpoch = 0;  // defined from the take, not by continuous play: B is already set
  p.anchorCount = 0;
  p.stateJournaled = p.stateFiled = false;
  if (curAnchorSlot_ == slot) curAnchorSlot_ = -1;
  touch();
  return Status::Ok();
}

Status Session::practiceGotoA(int slot) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  Status err;
  if (!checkSlot(slot, err)) return err;
  PracticeSlot& p = slots_[size_t(slot)];
  if (!p.used) return Error(Err::NotFound, "practice slot " + std::to_string(slot) + " has no A");
  bool entered = false;
  if (mode_ != Mode::Practice) {
    RN_TRY(enterPractice());
    entered = true;
  }
  Status st = core_->loadState(p.state.data(), p.state.size());
  if (!st.ok()) {
    if (entered) {
      Status back = leavePractice(returnMode_);
      if (!back.ok()) return back;
    } else {
      // The practice machine state is unknown now: restart the run from the saved take state.
      ++emuEpoch_;
      if (core_->loadState(returnState_.data(), returnState_.size()).ok()) resetPracticeRun(returnState_);
    }
    return Error(st.code, "practice slot " + std::to_string(slot) + " A state failed to load: " + st.message);
  }
  ++emuEpoch_;
  resetPracticeRun(p.state);
  p.anchorEpoch = emuEpoch_;
  p.anchorCount = emuCount_;
  curAnchorSlot_ = slot;
  curAnchorEpoch_ = emuEpoch_;
  curAnchorCount_ = emuCount_;
  audioValid_ = false;
  return Status::Ok();
}

Status Session::practiceRename(int slot, const std::string& name) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  Status err;
  if (!checkSlot(slot, err)) return err;
  PracticeSlot& p = slots_[size_t(slot)];
  if (!p.used) return Error(Err::NotFound, "practice slot " + std::to_string(slot) + " is empty");
  p.name = name;
  p.updatedSeq = ++practiceSeq_;
  touch();
  return Status::Ok();
}

Status Session::practiceClear(int slot) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  Status err;
  if (!checkSlot(slot, err)) return err;
  if (!slots_[size_t(slot)].used) return Status::Ok();
  slots_[size_t(slot)] = PracticeSlot();
  if (curAnchorSlot_ == slot) curAnchorSlot_ = -1;  // counter origin is kept
  touch();
  return Status::Ok();
}

uint64_t Session::practiceCounter() const {
  if (curAnchorEpoch_ != emuEpoch_ || emuCount_ < curAnchorCount_) return 0;
  return emuCount_ - curAnchorCount_;
}

PracticeStatus Session::practiceStatus() const {
  PracticeStatus ps;
  ps.active = mode_ == Mode::Practice;
  bool live = curAnchorEpoch_ == emuEpoch_ && emuCount_ >= curAnchorCount_;
  ps.anchorSlot = live ? curAnchorSlot_ : -1;
  ps.counter = practiceCounter();
  ps.returnFrame = frame_;
  ps.rewindAvailable = ps.active ? emuCount_ - practiceFloor() : 0;
  return ps;
}

// ------------------------------------------------------------------ persistence
Status Session::save() {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (!store_) return Error(Err::InvalidArg, "session has no project directory (use saveAs)");
  RN_TRY(store_->fullSave(*this));
  savedSeq_ = changeSeq_;
  return Status::Ok();
}

Status Session::reset(bool keepPracticeSlots) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (mode_ == Mode::Practice) return Error(Err::WrongMode, "cannot reset the project while practicing (leave practice first)");
  // The previous content, restored if the reset cannot be committed.
  Timeline oldTl = std::move(tl_);
  CheckpointStore oldCps = std::move(cps_);
  std::vector<Bookmark> oldBookmarks = std::move(bookmarks_);
  std::array<PracticeSlot, kPracticeSlots> oldSlots = slots_;
  const Mode oldMode = mode_;
  const uint64_t oldFrame = frame_;
  const int oldAnchor = curAnchorSlot_;
  const uint64_t oldChangeSeq = changeSeq_;

  // Fresh timeline / checkpoint store that continue the id counters: files named by segment and
  // checkpoint id (and practice states by sequence number) are never overwritten, so the
  // committed generation stays readable until the new index replaces it.
  tl_ = Timeline();
  tl_.setCounters(oldTl.nextId(), oldTl.seq());
  cps_ = CheckpointStore(opt_.checkpoints);
  cps_.setNextId(oldCps.nextId());
  bookmarks_.clear();
  for (auto& p : slots_) {
    if (!keepPracticeSlots) {
      p = PracticeSlot();
      continue;
    }
    // The A state stays valid; the take it was set on is gone.
    p.hasTakeFrame = false;
    p.takeFrame = 0;
    p.takeId = 0;
    p.anchorEpoch = p.anchorCount = 0;
  }
  curAnchorSlot_ = -1;
  mode_ = Mode::Record;
  ++emuEpoch_;  // continuity broken (A/B anchors)
  Status st = powerOnFresh();
  touch();

  auto rollback = [&]() {
    tl_ = std::move(oldTl);
    cps_ = std::move(oldCps);
    bookmarks_ = std::move(oldBookmarks);
    slots_ = oldSlots;
    mode_ = oldMode;
    curAnchorSlot_ = oldAnchor;
    changeSeq_ = oldChangeSeq;
    frame_ = 0;
    Status rs = powerOnFresh();
    if (rs.ok()) rs = resync(oldFrame, true);
    return rs;
  };
  if (!st.ok()) {
    rollback();
    return st;
  }
  if (store_) {
    const uint64_t gen = store_->generation();
    st = store_->fullSave(*this);
    if (!st.ok()) {
      // Before the index commit nothing on disk changed (only unreferenced new files): keep the
      // old project. After it the reset is committed; the next save finishes the remaining steps.
      if (store_->generation() == gen) rollback();
      return st;
    }
    savedSeq_ = changeSeq_;
  }
  return Status::Ok();
}

Status Session::autosave() {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (!store_) return Error(Err::InvalidArg, "session has no project directory (use saveAs)");
  return store_->autosave(*this);
}

Status Session::saveAs(const std::string& dir) {
  clearRewindCache();
  if (mode_ == Mode::Live) return Error(Err::UnsupportedFormat, "recording, persistence and timeline operations are unavailable in live mode");
  if (store_) return Error(Err::AlreadyExists, "session already has a project directory");
  std::unique_ptr<ProjectStore> st;
  RN_TRY(ProjectStore::create(dir, *this, st));
  store_ = std::move(st);
  savedSeq_ = changeSeq_;
  return Status::Ok();
}

}  // namespace rn
