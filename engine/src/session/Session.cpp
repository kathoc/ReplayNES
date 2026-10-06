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
  s->core_ = createCore(opt.core);
  s->rom_ = std::move(rom);
  s->romPath_ = romPath;
  s->romSha_ = Sha256::hex(s->rom_.data(), s->rom_.size());
  RN_TRY(s->powerOnFresh());
  out = std::move(s);
  return Status::Ok();
}

std::string Session::projectDir() const { return store_ ? store_->dir() : std::string(); }

Status Session::powerOnFresh() {
  RN_TRY(core_->loadROM(rom_.data(), rom_.size()));
  frame_ = 0;
  audioValid_ = false;
  return Status::Ok();
}

uint64_t Session::audioHashOf(const int16_t* a, size_t n) { return Hasher64::of(a, n * 2, n); }

const int16_t* Session::audio(size_t* count) const {
  const int16_t* a = core_->audio(count);
  if (!audioValid_) *count = 0;
  return a;
}

void Session::maybeCheckpoint() {
  const CheckpointPolicy& p = cps_.policy();
  bool sparse = frame_ % p.sparseInterval == 0;
  bool dense = frame_ % p.denseInterval == 0;
  if (!sparse && !dense) return;
  uint64_t owner = tl_.stateOwner(frame_);
  if (sparse) {
    if (cps_.find(frame_, owner, int(CpKind::Sparse))) return;
  } else if (cps_.find(frame_, owner)) {
    return;
  }
  std::vector<uint8_t> st;
  if (!captureState(st).ok()) return;  // a missing cache entry only costs seek time
  cps_.add(frame_, owner, sparse ? CpKind::Sparse : CpKind::Dense, core_->compatId(), core_->stateFormatVersion(),
           std::move(st));
}

Status Session::step(uint8_t p1, uint8_t p2, uint8_t events, StepInfo* info) {
  StepInfo si;
  si.mode = mode_;
  InputRecord r;
  if (mode_ == Mode::Practice) {
    if (events & ~kEvMask) return Error(Err::InvalidArg, "unknown event flags");
    r.p1 = p1;
    r.p2 = p2;
    r.events = events;
    RN_TRY(core_->stepRecord(r.p1, r.p2, r.events, true));
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
    r.p1 = p1;
    r.p2 = p2;
    r.events = events;
  }
  // Emulate first; only a frame that was really emulated gets recorded.
  RN_TRY(core_->stepRecord(r.p1, r.p2, r.events, true));
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
  maybeCheckpoint();
  si.frame = frame_;
  si.applied = r;
  si.take = tl_.head();
  si.endOfTake = mode_ == Mode::Replay && frame_ >= tl_.length();
  if (info) *info = si;
  return Status::Ok();
}

Status Session::resync(uint64_t target, bool forceReload) {
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
    RN_TRY(core_->stepRecord(r->p1, r->p2, r->events, frame_ + 1 == target));
    ++frame_;
    maybeCheckpoint();
  }
  audioValid_ = false;  // seeking is silent
  return Status::Ok();
}

Status Session::seek(uint64_t target) {
  if (mode_ == Mode::Practice) return Error(Err::WrongMode, "cannot seek the take while practicing (leave practice first)");
  RN_TRY(resync(target, false));
  touch();
  return Status::Ok();
}

Status Session::rewind(uint64_t n) {
  if (mode_ == Mode::Practice) return practiceRewind(n);
  return seek(frame_ - (n > frame_ ? frame_ : n));
}

// ------------------------------------------------------------------ bookmarks
Status Session::bookmarkAdd(const std::string& name, uint64_t* id) {
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
  Bookmark* b = findBm(bookmarks_, id);
  if (!b) return Error(Err::NotFound, "bookmark not found");
  b->name = name;
  touch();
  return Status::Ok();
}

Status Session::bookmarkGoto(uint64_t id) {
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
  for (auto& kv : tl_.segments()) {
    const Segment& s = kv.second;
    out.push_back({s.id, s.parent, s.start, s.end(), s.createdSeq, s.id == tl_.head(), tl_.childCount(s.id)});
  }
  return out;
}

Status Session::activateTake(uint64_t id) {
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
  if (mode_ == Mode::Practice) return Error(Err::WrongMode, "cannot switch takes while practicing");
  if (tl_.undo().empty()) return Error(Err::NotFound, "nothing to undo");
  UndoEntry e = tl_.undo().back();
  tl_.undo().pop_back();
  RN_TRY(tl_.setHead(e.head));
  uint64_t target = e.frame < tl_.length() ? e.frame : tl_.length();
  touch();
  return resync(target, true);
}

// ------------------------------------------------------------------ practice
Status Session::setMode(Mode m) {
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
  RN_TRY(core_->loadState(snap.state.data(), snap.state.size()));
  for (uint64_t c = snap.count; c < target; ++c) {
    const InputRecord& r = pInputs_[size_t(c - inputBase)];
    RN_TRY(core_->stepRecord(r.p1, r.p2, r.events, c + 1 == target));
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
      RN_TRY(core_->loadROM(rom_.data(), rom_.size()));  // fresh power-on (video buffer untouched below)
      frame_ = 0;
    }
  }
  while (frame_ < target) {
    const InputRecord* r = tl_.recordAt(frame_);
    if (!r) return Error(Err::Internal, "missing record while locating A");
    RN_TRY(core_->stepRecord(r->p1, r->p2, r->events, false));
    ++frame_;
    maybeCheckpoint();
  }
  return captureState(out);
}

Status Session::practiceSetRange(int slot, uint64_t a, uint64_t b) {
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
    std::vector<uint32_t> savedVideo(core_->video(), core_->video() + size_t(kVideoWidth) * kVideoHeight);
    RN_TRY(captureState(saved));
    const uint64_t savedFrame = frame_, savedEpoch = emuEpoch_;
    Status walk = takeStateAt(a, stA);
    // Frame 0 is restored like everywhere else: a fresh power-on (bit-identical to a new instance).
    Status back = savedFrame == 0 ? powerOnFresh() : core_->loadState(saved.data(), saved.size());
    frame_ = savedFrame;
    audioValid_ = false;
    bool rebuild = !back.ok() ||  // never leave the session on a wrong state
                   std::memcmp(core_->video(), savedVideo.data(), savedVideo.size() * 4) != 0;  // picture cleared
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
  if (!store_) return Error(Err::InvalidArg, "session has no project directory (use saveAs)");
  RN_TRY(store_->fullSave(*this));
  savedSeq_ = changeSeq_;
  return Status::Ok();
}

Status Session::reset(bool keepPracticeSlots) {
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
  if (!store_) return Error(Err::InvalidArg, "session has no project directory (use saveAs)");
  return store_->autosave(*this);
}

Status Session::saveAs(const std::string& dir) {
  if (store_) return Error(Err::AlreadyExists, "session already has a project directory");
  std::unique_ptr<ProjectStore> st;
  RN_TRY(ProjectStore::create(dir, *this, st));
  store_ = std::move(st);
  savedSeq_ = changeSeq_;
  return Status::Ok();
}

}  // namespace rn
