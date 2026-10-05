#include "core/MockCore.h"

#include "util/Bytes.h"
#include "util/Hash.h"

namespace rn {

MockCore::MockCore() : video_(size_t(kVideoWidth) * kVideoHeight, 0xFF000000u), audio_(kMaxSamplesPerFrame, 0) {}

Status MockCore::loadROM(const uint8_t* data, size_t size) {
  if (!data || size == 0) return Error(Err::RomInvalid, "empty ROM");
  romSeed_ = Hasher64::of(data, size);
  loaded_ = true;
  frame_ = 0;
  powers_ = resets_ = 0;
  return powerCycle();
}

Status MockCore::powerCycle() {
  if (!loaded_) return Error(Err::InvalidArg, "no ROM loaded");
  acc_ = romSeed_;
  ++powers_;
  return Status::Ok();
}

Status MockCore::softReset() {
  if (!loaded_) return Error(Err::InvalidArg, "no ROM loaded");
  ++resets_;
  acc_ ^= 0xA5A5A5A5u + resets_;
  return Status::Ok();
}

Status MockCore::stepFrame(uint8_t p1, uint8_t p2, bool renderVideo) {
  if (!loaded_) return Error(Err::InvalidArg, "no ROM loaded");
  acc_ = acc_ * 6364136223846793005ULL + 1442695040888963407ULL;
  acc_ ^= uint64_t(p1) << 8 | uint64_t(p2) << 24 | uint64_t(powers_) << 40;
  if (renderVideo) {
    videoAcc_ = acc_;
    videoDirty_ = true;
  }
  audioCount_ = audioSamplesForFrame(frame_);
  int16_t base = int16_t(acc_ >> 48);
  for (size_t i = 0; i < audioCount_; ++i) audio_[i] = int16_t(base + int16_t(i * 37));
  ++frame_;
  return Status::Ok();
}

const uint32_t* MockCore::video() const {
  if (videoDirty_) {
    uint32_t c = 0xFF000000u | uint32_t(videoAcc_ >> 40);
    for (size_t i = 0; i < video_.size(); ++i) video_[i] = c ^ uint32_t(i & 0xFF);
    videoDirty_ = false;
  }
  return video_.data();
}

Status MockCore::saveState(std::vector<uint8_t>& out) {
  if (!loaded_) return Error(Err::InvalidArg, "no ROM loaded");
  ByteWriter w;
  w.u64(acc_);
  w.u64(romSeed_);
  w.u32(resets_);
  w.u32(powers_);
  stateenv::write(out, kCompatId, frame_, w.buf.data(), w.buf.size());
  return Status::Ok();
}

Status MockCore::loadState(const uint8_t* data, size_t size) {
  uint64_t frame;
  const uint8_t* p;
  size_t n;
  RN_TRY(stateenv::read(data, size, kCompatId, frame, p, n));
  ByteReader r(p, n);
  uint64_t acc = r.u64(), seed = r.u64();
  uint32_t resets = r.u32(), powers = r.u32();
  if (!r.ok() || r.remaining()) return Error(Err::StateError, "bad mock state");
  if (loaded_ && seed != romSeed_) return Error(Err::StateError, "state belongs to a different ROM");
  acc_ = acc; romSeed_ = seed; resets_ = resets; powers_ = powers; frame_ = frame; loaded_ = true;
  audioCount_ = 0;
  return Status::Ok();
}

}  // namespace rn
