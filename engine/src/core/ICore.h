// ICore: the only interface the rest of the engine uses to talk to an emulation core.
// Contract (all implementations):
//  * One instance = one machine. No shared mutable state between instances that can
//    influence emulation results (see docs/ARCHITECTURE_DECISION.md for Nestopia statics).
//  * frameIndex() = logical frames emulated since loadROM (NOT reset by power cycle).
//  * Full-capability cores: stepFrame emits exactly samplesForFrame(frameIndex) sample frames
//    (mediaInfo().audioChannels interleaved int16 values each; the NES cores: mono 48 kHz), so
//    audio sample positions are a pure function of the frame index.
//  * CapState cores: saveState/loadState round-trip the complete machine state including frameIndex.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "input/InputLayout.h"
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

// What a core produces for the loaded content. The defaults are the NES values above; a core for
// another system overrides ICore::mediaInfo / videoFrame / samplesBefore.
struct MediaInfo {
  uint32_t maxWidth = kVideoWidth, maxHeight = kVideoHeight;  // capacity of video(); fixed per content
  uint64_t fpsNum = kFpsNum, fpsDen = kFpsDen;                // exact emulated frame rate
  uint32_t audioChannels = 1;                                 // interleaved int16 values per sample frame
  uint32_t pixelAspectNum = 8, pixelAspectDen = 7;            // display width / height of one pixel
  uint32_t rotation = 0;                                      // display rotation, quarter turns clockwise
};
// The picture of the last emulated frame: width x height pixels, rows `stride` pixels apart.
struct VideoFrame {
  const uint32_t* pixels = nullptr;
  uint32_t width = 0, height = 0, stride = 0;
};

// System event flags (applied before emulating the frame they are recorded on).
enum : uint8_t { kEvSoftReset = 0x01, kEvPowerCycle = 0x02, kEvMask = 0x03 };

// External: a core the product registers at start-up (registerExternalCore), for example an arcade
// core in another product built from these sources.
enum class CoreKind : int { Nestopia = 0, Mock = 1, ReplayNEScore = 2, External = 3 };

// Cores without CapAudio return nullptr/count=0; absent state/reset return UnsupportedFormat.
class ICore {
 public:
  virtual ~ICore() = default;
  virtual Status loadROM(const uint8_t* data, size_t size) = 0;  // loads + powers on, frame=0
  virtual Status powerCycle() = 0;
  virtual Status softReset() = 0;
  // renderVideo=false skips only the final pixel conversion; emulation is identical.
  virtual Status stepFrame(uint8_t p1, uint8_t p2, bool renderVideo = true) = 0;
  // One frame with the game input of `r` (the bytes inputLayout() declares; events are applied by
  // stepRecord). Default: the NES pads. A core with another layout overrides both.
  virtual Status stepInput(const InputRecord& r, bool renderVideo = true) { return stepFrame(r.p1, r.p2, renderVideo); }
  virtual InputLayout inputLayout() const { return InputLayout::nes(); }
  virtual uint64_t frameIndex() const = 0;
  virtual const uint32_t* video() const = 0;  // kVideoWidth*kVideoHeight BGRA8 (0xAARRGGBB LE), alpha=255
  // Sample frames of the last stepFrame: *count frames of mediaInfo().audioChannels int16 values.
  virtual const int16_t* audio(size_t* count) const = 0;
  virtual MediaInfo mediaInfo() const { return MediaInfo{}; }
  virtual VideoFrame videoFrame() const { return VideoFrame{video(), kVideoWidth, kVideoHeight, kVideoWidth}; }
  // Sample frames emitted before logical frame `frame`: a pure function of the frame index, so
  // audio positions follow from frame numbers alone. Default: the NTSC formula above.
  virtual uint64_t samplesBefore(uint64_t frame) const { return audioSamplesBefore(frame); }
  uint32_t samplesForFrame(uint64_t frame) const { return uint32_t(samplesBefore(frame + 1) - samplesBefore(frame)); }
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
  // Debug / test side channel: the 2 KiB of CPU work RAM ($0000-$07FF), read-only. Never part of
  // the emulation path. nullptr = not supported (mock core).
  virtual const uint8_t* cpuRam() const { return nullptr; }
  virtual Status saveState(std::vector<uint8_t>& out) = 0;
  virtual Status loadState(const uint8_t* data, size_t size) = 0;
  virtual std::string compatId() const = 0;    // changes whenever replay results could change
  virtual std::string buildId() const = 0;     // informational (exact source revision)
  virtual uint32_t stateFormatVersion() const = 0;
  virtual CoreKind kind() const = 0;

  // Convenience: apply event flags then step. Power cycle wins over soft reset.
  Status stepRecord(const InputRecord& r, bool renderVideo = true) {
    if (r.events & kEvPowerCycle) RN_TRY(powerCycle());
    else if (r.events & kEvSoftReset) RN_TRY(softReset());
    return stepInput(r, renderVideo);
  }
  Status stepRecord(uint8_t p1, uint8_t p2, uint8_t events, bool renderVideo = true) {
    if (events & kEvPowerCycle) RN_TRY(powerCycle());
    else if (events & kEvSoftReset) RN_TRY(softReset());
    return stepFrame(p1, p2, renderVideo);
  }
  uint64_t machineHash();
  uint64_t videoHash() const;
  uint64_t audioHash() const;
};

// The product's own core (CoreKind::External). Register once at start-up, before any session or
// renderer exists; not thread-safe against concurrent core creation.
struct ExternalCore {
  std::string name;      // stable name (coreName)
  std::string compatId;  // changes whenever replay results could change (coreCompatId)
  uint32_t capabilities = 0;
  std::function<std::unique_ptr<ICore>()> create;
};
void registerExternalCore(ExternalCore core);
void unregisterExternalCore();

// Hashes used for verification: the visible picture, and sample frames of `channels` values.
uint64_t videoFrameHash(const VideoFrame& v);
uint64_t audioFramesHash(const int16_t* a, size_t frames, uint32_t channels);

enum CoreCapability : uint32_t { CapAudio = 1, CapState = 2, CapSoftReset = 4, CapRecording = 8 };
const char* coreName(CoreKind kind);
bool coreAvailable(CoreKind kind);
uint32_t coreCapabilities(CoreKind kind);
bool coreForCompatId(const std::string& compat, CoreKind& kind);
// Unknown or unavailable kinds return nullptr; never substitute a different core.
std::unique_ptr<ICore> createCore(CoreKind kind);
std::string coreCompatId(CoreKind kind);

// Wrapper state envelope shared by implementations.
namespace stateenv {
void write(std::vector<uint8_t>& out, const std::string& compat, uint64_t frame, const uint8_t* payload, size_t n);
Status read(const uint8_t* data, size_t n, const std::string& expectCompat, uint64_t& frame, const uint8_t*& payload,
            size_t& payloadLen);
}  // namespace stateenv

}  // namespace rn
