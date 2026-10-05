// MockCore: deterministic fake machine for fast engine tests (timeline, persistence, long runs).
// State = frame counter, reset/power counters and a 64-bit accumulator mixed with every input.
#pragma once
#include "core/ICore.h"

namespace rn {

class MockCore final : public ICore {
 public:
  static constexpr const char* kCompatId = "mock-core/1";
  MockCore();
  Status loadROM(const uint8_t* data, size_t size) override;
  Status powerCycle() override;
  Status softReset() override;
  Status stepFrame(uint8_t p1, uint8_t p2, bool renderVideo) override;
  uint64_t frameIndex() const override { return frame_; }
  const uint32_t* video() const override;  // rendered lazily (cheap long runs)
  const int16_t* audio(size_t* count) const override { *count = audioCount_; return audio_.data(); }
  Status saveState(std::vector<uint8_t>& out) override;
  Status loadState(const uint8_t* data, size_t size) override;
  std::string compatId() const override { return kCompatId; }
  std::string buildId() const override { return "mock"; }
  uint32_t stateFormatVersion() const override { return 1; }
  CoreKind kind() const override { return CoreKind::Mock; }
  uint64_t accumulator() const { return acc_; }

 private:
  bool loaded_ = false;
  uint64_t frame_ = 0, acc_ = 0, romSeed_ = 0;
  uint32_t resets_ = 0, powers_ = 0;
  mutable std::vector<uint32_t> video_;
  mutable bool videoDirty_ = false;
  uint64_t videoAcc_ = 0;
  std::vector<int16_t> audio_;
  size_t audioCount_ = 0;
};

}  // namespace rn
