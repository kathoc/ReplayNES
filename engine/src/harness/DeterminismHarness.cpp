#include "harness/DeterminismHarness.h"

#include <chrono>

#include "session/Session.h"

namespace rn {

static Status runLoop(ICore& core, const std::vector<InputRecord>& script, uint64_t from, uint32_t stateEvery,
                      Trace& out, uint64_t captureAt, std::vector<uint8_t>* captured, const Perturb& perturb) {
  out.first = from;
  out.video.clear();
  out.audio.clear();
  out.state.clear();
  const uint64_t n = script.size();
  for (uint64_t f = from; f < n; ++f) {
    if (captured && f == captureAt) RN_TRY(core.saveState(*captured));
    InputRecord r = script[size_t(f)];
    if (perturb) perturb(f, r);
    RN_TRY(core.stepRecord(r.p1, r.p2, r.events, true));
    size_t cnt;
    const int16_t* a = core.audio(&cnt);
    out.video.push_back(core.videoHash());
    out.audio.push_back(Session::audioHashOf(a, cnt));
    uint64_t sf = f + 1;
    if (sf % stateEvery == 0 || sf == n) out.state[sf] = core.machineHash();
  }
  return Status::Ok();
}

Status DeterminismHarness::runFromStart(CoreKind kind, const std::vector<uint8_t>& rom,
                                        const std::vector<InputRecord>& script, uint32_t stateEvery, Trace& out,
                                        uint64_t captureAt, std::vector<uint8_t>* captured, const Perturb& perturb) {
  auto core = createCore(kind);
  RN_TRY(core->loadROM(rom.data(), rom.size()));
  return runLoop(*core, script, 0, stateEvery, out, captureAt, captured, perturb);
}

Status DeterminismHarness::runFromState(CoreKind kind, const std::vector<uint8_t>& rom,
                                        const std::vector<InputRecord>& script, const std::vector<uint8_t>& state,
                                        uint32_t stateEvery, Trace& out) {
  auto core = createCore(kind);
  RN_TRY(core->loadROM(rom.data(), rom.size()));
  RN_TRY(core->loadState(state.data(), state.size()));
  return runLoop(*core, script, core->frameIndex(), stateEvery, out, 0, nullptr, nullptr);
}

Divergence DeterminismHarness::compare(const Trace& a, const Trace& b, bool compareAudio) {
  Divergence d;
  uint64_t from = a.first > b.first ? a.first : b.first;
  uint64_t endA = a.first + a.video.size(), endB = b.first + b.video.size();
  uint64_t to = endA < endB ? endA : endB;
  for (uint64_t f = from; f < to; ++f) {
    // State hash "after frame f" is keyed f+1.
    auto sa = a.state.find(f + 1), sb = b.state.find(f + 1);
    bool stateDiff = sa != a.state.end() && sb != b.state.end() && sa->second != sb->second;
    bool videoDiff = a.video[size_t(f - a.first)] != b.video[size_t(f - b.first)];
    bool audioDiff = compareAudio && a.audio[size_t(f - a.first)] != b.audio[size_t(f - b.first)];
    if (videoDiff || audioDiff || stateDiff) {
      d.found = true;
      d.frame = f;
      d.component = videoDiff ? "video" : audioDiff ? "audio" : "state";
      d.detail = "first divergence at emulated frame " + std::to_string(f) + " (" + d.component + ")";
      return d;
    }
  }
  if (endA != endB) {
    d.found = true;
    d.frame = to;
    d.component = "length";
    d.detail = "traces have different lengths";
  }
  return d;
}

Status DeterminismHarness::run(const HarnessConfig& cfg, const std::vector<uint8_t>& rom,
                               const std::vector<InputRecord>& script, HarnessReport& report,
                               const Perturb& perturbSecondRun) {
  auto t0 = std::chrono::steady_clock::now();
  report = HarnessReport();
  if (cfg.runs < 1 || cfg.stateEvery == 0) return Error(Err::InvalidArg, "bad harness config");
  if (cfg.midFrame >= script.size() && cfg.midFrame != 0) return Error(Err::InvalidArg, "midFrame beyond script");
  Trace base;
  std::vector<uint8_t> mid;
  RN_TRY(runFromStart(cfg.core, rom, script, cfg.stateEvery, base, cfg.midFrame, cfg.midFrame ? &mid : nullptr));
  report.frames += script.size();
  for (int i = 1; i < cfg.runs && !report.runs.found; ++i) {
    Trace t;
    RN_TRY(runFromStart(cfg.core, rom, script, cfg.stateEvery, t, 0, nullptr, i == 1 ? perturbSecondRun : nullptr));
    report.frames += script.size();
    report.runs = compare(base, t, true);
  }
  if (cfg.midFrame) {
    Trace t;
    RN_TRY(runFromState(cfg.core, rom, script, mid, cfg.stateEvery, t));
    report.frames += script.size() - cfg.midFrame;
    report.mid = compare(base, t, cfg.compareAudioAfterLoad);
  }
  report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return Status::Ok();
}

std::vector<InputRecord> DeterminismHarness::randomScript(uint64_t frames, uint64_t seed, uint64_t resetEvery,
                                                          uint32_t holdFrames) {
  std::vector<InputRecord> s;
  s.reserve(size_t(frames));
  uint64_t x = seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL;
  auto nextRand = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  InputRecord cur;
  for (uint64_t f = 0; f < frames; ++f) {
    if (holdFrames == 0 || f % holdFrames == 0) {
      uint64_t r = nextRand();
      cur.p1 = uint8_t(r);
      cur.p2 = uint8_t(r >> 8);
    }
    InputRecord rec = cur;
    rec.events = 0;
    if (resetEvery && f > 0 && f % resetEvery == 0) rec.events = (f / resetEvery) % 2 ? kEvSoftReset : kEvPowerCycle;
    s.push_back(rec);
  }
  return s;
}

}  // namespace rn
