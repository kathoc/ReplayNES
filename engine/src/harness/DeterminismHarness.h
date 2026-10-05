// Determinism harness: runs an input script on fresh core instances and compares per-frame
// video/audio hashes and periodic machine-state hashes; reports the first divergent frame and
// component. Also compares a run resumed from a mid-script savestate against a run from power-on.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/ICore.h"
#include "timeline/Timeline.h"

namespace rn {

struct Trace {
  uint64_t first = 0;               // frame index of video[0]/audio[0]
  std::vector<uint64_t> video;      // per emulated frame
  std::vector<uint64_t> audio;
  std::map<uint64_t, uint64_t> state;  // frame (state after `frame` frames) -> machine hash
};

struct Divergence {
  bool found = false;
  uint64_t frame = 0;     // emulated frame index (or state frame) where it was first seen
  std::string component;  // "state" | "video" | "audio" | "length"
  std::string detail;
};

// Hook to inject faults in tests: may modify the record before frame `frame` is emulated.
using Perturb = std::function<void(uint64_t frame, InputRecord& rec)>;

struct HarnessConfig {
  CoreKind core = CoreKind::Nestopia;
  uint32_t stateEvery = 60;   // machine-state hash interval (also hashed at the last frame)
  int runs = 2;               // independent runs from power-on
  uint64_t midFrame = 0;      // 0 = no mid-savestate run
  // Compare PCM of the resumed run too. Bit-exact with the patched core (cmake/NestopiaPatches.cmake,
  // patch 2 persists the APU output stage); upstream Nestopia shifts PCM by ~1 sample after a load.
  bool compareAudioAfterLoad = true;
};

struct HarnessReport {
  Divergence runs;  // first divergence among power-on runs (vs run 0)
  Divergence mid;   // mid-savestate run vs run 0
  uint64_t frames = 0;
  double seconds = 0;
};

class DeterminismHarness {
 public:
  static Status runFromStart(CoreKind kind, const std::vector<uint8_t>& rom, const std::vector<InputRecord>& script,
                             uint32_t stateEvery, Trace& out, uint64_t captureAt = 0,
                             std::vector<uint8_t>* captured = nullptr, const Perturb& perturb = nullptr);
  static Status runFromState(CoreKind kind, const std::vector<uint8_t>& rom, const std::vector<InputRecord>& script,
                             const std::vector<uint8_t>& state, uint32_t stateEvery, Trace& out);
  static Divergence compare(const Trace& a, const Trace& b, bool compareAudio);
  static Status run(const HarnessConfig& cfg, const std::vector<uint8_t>& rom, const std::vector<InputRecord>& script,
                    HarnessReport& report, const Perturb& perturbSecondRun = nullptr);
  // Deterministic pseudo-random input script (xorshift); events every `resetEvery` frames
  // (alternating soft reset / power cycle) when > 0.
  static std::vector<InputRecord> randomScript(uint64_t frames, uint64_t seed, uint64_t resetEvery = 0,
                                               uint32_t holdFrames = 6);
};

}  // namespace rn
