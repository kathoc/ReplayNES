#pragma once
#include "core/ICore.h"
namespace rn {
// Experimental live-only bridge. Pulse/triangle PCM and state codec exist; recording/soft reset do not.
class ReplayNEScoreAdapter final : public ICore {
 public:
  ReplayNEScoreAdapter();
  ~ReplayNEScoreAdapter() override;
  static std::string staticCompatId() { return "replaynescore/nrom-live/state-v7"; }
  Status loadROM(const uint8_t*, size_t) override;
  Status powerCycle() override;
  Status softReset() override;
  Status stepFrame(uint8_t, uint8_t, bool = true) override;
  uint64_t frameIndex() const override;
  const uint32_t* video() const override;
  const int16_t* audio(size_t* count) const override;
  const uint8_t* cpuRam() const override;
  Status saveState(std::vector<uint8_t>&) override;
  Status loadState(const uint8_t*, size_t) override;
  std::string compatId() const override { return staticCompatId(); }
  std::string buildId() const override { return "experimental-nrom-live-v7"; }
  uint32_t stateFormatVersion() const override { return 1; }
  CoreKind kind() const override { return CoreKind::ReplayNEScore; }
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
