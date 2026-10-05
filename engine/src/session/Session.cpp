#include "session/Session.h"

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
  RN_TRY(resync(target, false));
  touch();
  return Status::Ok();
}

// ------------------------------------------------------------------ bookmarks
Status Session::bookmarkAdd(const std::string& name, uint64_t* id) {
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
  if (!tl_.segment(id)) return Error(Err::NotFound, "take not found");
  if (id == tl_.head()) return Status::Ok();
  tl_.undo().push_back({tl_.head(), frame_});
  RN_TRY(tl_.setHead(id));
  uint64_t target = frame_ < tl_.length() ? frame_ : tl_.length();
  touch();
  return resync(target, true);
}

Status Session::undoTakeSwitch() {
  if (tl_.undo().empty()) return Error(Err::NotFound, "nothing to undo");
  UndoEntry e = tl_.undo().back();
  tl_.undo().pop_back();
  RN_TRY(tl_.setHead(e.head));
  uint64_t target = e.frame < tl_.length() ? e.frame : tl_.length();
  touch();
  return resync(target, true);
}

// ------------------------------------------------------------------ persistence
Status Session::save() {
  if (!store_) return Error(Err::InvalidArg, "session has no project directory (use saveAs)");
  RN_TRY(store_->fullSave(*this));
  savedSeq_ = changeSeq_;
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
