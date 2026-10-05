#include "render/OfflineRenderer.h"

namespace rn {

Status OfflineRenderer::create(const Session& s, uint64_t start, uint64_t end, std::unique_ptr<OfflineRenderer>& out) {
  uint64_t len = s.takeLength();
  if (end == 0) end = len;
  if (end > len || start >= end) return Error(Err::OutOfRange, "render range outside the active take");
  std::unique_ptr<OfflineRenderer> r(new OfflineRenderer());
  r->recs_ = s.timeline().flattenActive();  // copy: the session may keep changing
  r->rom_ = s.rom();
  r->core_ = createCore(s.coreKind());
  RN_TRY(r->core_->loadROM(r->rom_.data(), r->rom_.size()));
  r->start_ = start;
  r->end_ = end;
  out = std::move(r);
  return Status::Ok();
}

Status OfflineRenderer::next(const uint32_t** video, const int16_t** audio, size_t* samples, uint64_t* frameIndex) {
  if (!started_) {
    for (; pos_ < start_; ++pos_) {
      const InputRecord& r = recs_[size_t(pos_)];
      RN_TRY(core_->stepRecord(r.p1, r.p2, r.events, false));
    }
    started_ = true;
  }
  if (pos_ >= end_) return Error(Err::EndOfTake, "render finished");
  const InputRecord& r = recs_[size_t(pos_)];
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
