// NestopiaCore: ICore over the Nestopia UE instance API (Nes::Api::Emulator).
// Deterministic configuration pinned here (see docs/COMPATIBILITY.md, "adapter settings"):
//   NTSC NES (RP2C02), RAM power-on state 0x00 (never the rand() option), YUV palette with the
//   canonical decoder and all picture controls at 0, no NTSC filter, 48000 Hz mono 16-bit with the
//   optional output low/high-pass filter OFF (its float state is not saved in states),
//   standard pads on ports 1/2 with simultaneous opposite directions allowed (SOCD policy is
//   applied upstream by the InputPipeline and recorded), no image database, no user callbacks.
#pragma once
#include <memory>

#include "core/ICore.h"

namespace rn {

class NestopiaCore final : public ICore {
 public:
  // Bump when any adapter setting above changes (it changes replay results).
  static constexpr int kAdapterVersion = 1;
  static std::string staticCompatId();

  // Colour-burst phase b (0..2) of the picture just emulated, from the CPU's monotonic master-clock
  // counter at the frame boundary (Cpu::ticks: the sum of all frame lengths since power-on / reset,
  // saved and restored by states in the CPU 'CLK' chunk). The visible rows of a frame start at a
  // fixed distance before its end (the odd-frame dot skip happens in the pre-render line, before
  // row 0), so row 0's subcarrier phase is 8 * dots mod 12 = 4b with b = 2 * dots mod 3
  // (dots = master clocks / 4). In uninterrupted play from power-on this is exactly Nestopia's
  // Ppu::GetBurstPhase() (same values, same origin), but that counter is not in states and
  // Ppu::LoadState resets it to 0, so a frame reached by a seek / rewind / take switch / state
  // load used to report another phase than in straight play.
  static uint32_t burstPhaseAtFrameEnd(uint64_t masterClocks) { return uint32_t((masterClocks / 4) * 2 % 3); }

  NestopiaCore();
  ~NestopiaCore() override;
  Status loadROM(const uint8_t* data, size_t size) override;
  Status powerCycle() override;
  Status softReset() override;
  Status stepFrame(uint8_t p1, uint8_t p2, bool renderVideo) override;
  uint64_t frameIndex() const override { return frame_; }
  const uint32_t* video() const override { return video_.data(); }
  const int16_t* audio(size_t* count) const override { *count = audioCount_; return audio_.data(); }
  const uint16_t* videoCodes(uint32_t* burstPhase, uint64_t* frame) const override {
    if (burstPhase) *burstPhase = codesBurstPhase_;
    if (frame) *frame = codesFrame_;
    return codes_.data();
  }
  const uint8_t* cpuRam() const override;
  Status saveState(std::vector<uint8_t>& out) override;
  Status loadState(const uint8_t* data, size_t size) override;
  std::string compatId() const override { return staticCompatId(); }
  std::string buildId() const override;
  uint32_t stateFormatVersion() const override { return 1; }
  CoreKind kind() const override { return CoreKind::Nestopia; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool loaded_ = false;
  uint64_t frame_ = 0;
  std::vector<uint32_t> video_;
  std::vector<uint16_t> codes_;    // raw PPU codes of the picture in video_ (display-only copy)
  uint32_t codesBurstPhase_ = 0;
  uint64_t codesFrame_ = 0;
  std::vector<int16_t> audio_;
  size_t audioCount_ = 0;
};

}  // namespace rn
