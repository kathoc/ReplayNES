// ICore: the only interface the rest of the engine uses to talk to an emulation core.
// Contract (all implementations):
//  * One instance = one machine. No shared mutable state between instances that can
//    influence emulation results (see docs/ARCHITECTURE_DECISION.md for Nestopia statics).
//  * frameIndex() = logical frames emulated since loadROM (NOT reset by power cycle).
//  * stepFrame emits exactly audioSamplesForFrame(frameIndex) mono int16 samples @48 kHz,
//    so audio sample positions are a pure function of the frame index.
//  * saveState/loadState round-trip the complete machine state including frameIndex.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "util/Status.h"

namespace rn {

constexpr int kVideoWidth = 256;
constexpr int kVideoHeight = 240;
constexpr uint32_t kSampleRate = 48000;
// NTSC frame rate = 39375000/655171 Hz exactly.
constexpr uint64_t kFpsNum = 39375000;
constexpr uint64_t kFpsDen = 655171;
constexpr uint32_t kMaxSamplesPerFrame = 1024;

inline uint64_t audioSamplesBefore(uint64_t frame) { return frame * kSampleRate * kFpsDen / kFpsNum; }
inline uint32_t audioSamplesForFrame(uint64_t frame) {
  return uint32_t(audioSamplesBefore(frame + 1) - audioSamplesBefore(frame));
}

// System event flags (applied before emulating the frame they are recorded on).
enum : uint8_t { kEvSoftReset = 0x01, kEvPowerCycle = 0x02, kEvMask = 0x03 };

enum class CoreKind : int { Nestopia = 0, Mock = 1 };

class ICore {
 public:
  virtual ~ICore() = default;
  virtual Status loadROM(const uint8_t* data, size_t size) = 0;  // loads + powers on, frame=0
  virtual Status powerCycle() = 0;
  virtual Status softReset() = 0;
  // renderVideo=false skips only the final pixel conversion; emulation is identical.
  virtual Status stepFrame(uint8_t p1, uint8_t p2, bool renderVideo = true) = 0;
  virtual uint64_t frameIndex() const = 0;
  virtual const uint32_t* video() const = 0;  // kVideoWidth*kVideoHeight BGRA8 (0xAARRGGBB LE), alpha=255
  virtual const int16_t* audio(size_t* count) const = 0;  // samples of the last stepFrame
  // Display-only side channel for the CRT signal model: the raw PPU output codes of the frame in
  // video() (kVideoWidth*kVideoHeight, bits 0-5 = palette index after greyscale, bits 6-8 =
  // $2001 emphasis bits 5-7), the colour-burst phase of that frame (0..2, a function of the
  // machine state only: the same whether the frame was reached by straight play, a seek or a
  // state load) and the frameIndex() that produced it. Captured together with video(), so they
  // always describe the same picture. Never part of state or hashes. nullptr = not supported.
  virtual const uint16_t* videoCodes(uint32_t* burstPhase, uint64_t* frame) const {
    (void)burstPhase; (void)frame;
    return nullptr;
  }
  virtual Status saveState(std::vector<uint8_t>& out) = 0;
  virtual Status loadState(const uint8_t* data, size_t size) = 0;
  virtual std::string compatId() const = 0;    // changes whenever replay results could change
  virtual std::string buildId() const = 0;     // informational (exact source revision)
  virtual uint32_t stateFormatVersion() const = 0;
  virtual CoreKind kind() const = 0;

  // Convenience: apply event flags then step. Power cycle wins over soft reset.
  Status stepRecord(uint8_t p1, uint8_t p2, uint8_t events, bool renderVideo = true) {
    if (events & kEvPowerCycle) RN_TRY(powerCycle());
    else if (events & kEvSoftReset) RN_TRY(softReset());
    return stepFrame(p1, p2, renderVideo);
  }
  uint64_t machineHash();
  uint64_t videoHash() const;
  uint64_t audioHash() const;
};

std::unique_ptr<ICore> createCore(CoreKind kind);
std::string coreCompatId(CoreKind kind);

// Wrapper state envelope shared by implementations.
namespace stateenv {
void write(std::vector<uint8_t>& out, const std::string& compat, uint64_t frame, const uint8_t* payload, size_t n);
Status read(const uint8_t* data, size_t n, const std::string& expectCompat, uint64_t& frame, const uint8_t*& payload,
            size_t& payloadLen);
}  // namespace stateenv

}  // namespace rn
