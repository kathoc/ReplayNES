#include "render/OfflineRenderer.h"

namespace rn {

Status OfflineRenderer::create(const Session& s, uint64_t start, uint64_t end, std::unique_ptr<OfflineRenderer>& out) {
  uint64_t len = s.takeLength();
  if (end == 0) end = len;
  if (end > len || start >= end) return Error(Err::OutOfRange, "render range outside the active take");
  std::unique_ptr<OfflineRenderer> r(new OfflineRenderer());
  // Copies: the session may keep changing.
  const Checkpoint* cp = start > 0 ? s.checkpoints().best(start, s.timeline()) : nullptr;
  if (cp && cp->frame > 0) {
    r->seed_ = cp->data;
    r->recBase_ = cp->frame;
    r->recs_.reserve(size_t(end - cp->frame));
    for (uint64_t f = cp->frame; f < end; ++f) {
      const InputRecord* rec = s.timeline().recordAt(f);
      if (!rec) return Error(Err::Internal, "missing record for the render range");
      r->recs_.push_back(*rec);
    }
  } else {
    r->recs_ = s.timeline().flattenActive();
    r->recs_.resize(size_t(end));
  }
  r->rom_ = s.rom();
  r->kind_ = s.coreKind();
  r->start_ = start;
  r->end_ = end;
  r->pos_ = r->recBase_;
  out = std::move(r);
  return Status::Ok();
}

Status OfflineRenderer::init() {
  core_ = createCore(kind_);
  RN_TRY(core_->loadROM(rom_.data(), rom_.size()));
  if (!seed_.empty()) {
    Status st = core_->loadState(seed_.data(), seed_.size());
    if (!st.ok()) return Error(st.code, "render start checkpoint failed to load: " + st.message);
    seed_.clear();
    seed_.shrink_to_fit();
  }
  return Status::Ok();
}

Status OfflineRenderer::next(const uint32_t** video, const int16_t** audio, size_t* samples, uint64_t* frameIndex) {
  if (!started_) {
    if (!core_) RN_TRY(init());
    for (; pos_ < start_; ++pos_) {
      const InputRecord& r = recs_[size_t(pos_ - recBase_)];
      RN_TRY(core_->stepRecord(r.p1, r.p2, r.events, false));
    }
    started_ = true;
  }
  if (pos_ >= end_) return Error(Err::EndOfTake, "render finished");
  const InputRecord& r = recs_[size_t(pos_ - recBase_)];
  RN_TRY(core_->stepRecord(r.p1, r.p2, r.events, true));
  size_t n = 0;
  const int16_t* a = core_->audio(&n);
  running_.u64(core_->videoHash());
  running_.u64(Session::audioHashOf(a, n));
  if (video) *video = core_->video();
  if (audio) *audio = a;
  if (samples) *samples = n;
  if (frameIndex) *frameIndex = pos_;
  ++pos_;
  return Status::Ok();
}

}  // namespace rn
