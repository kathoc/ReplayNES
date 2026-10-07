#pragma once
#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/ICore.h"
#include "harness/DeterminismHarness.h"
#include "session/Session.h"
#include "testrom/TestRom.h"
#include "util/Fs.h"

#ifndef RN_TEST_TMP
#error "RN_TEST_TMP must point to a scratch directory"
#endif

namespace rntest {

// Scratch root: $RN_TEST_TMP at run time (tests copied to another machine, e.g. the Windows VM),
// else the build tree's tests/tmp baked in at compile time.
inline std::string tempRoot() {
  const char* env = std::getenv("RN_TEST_TMP");
  return env && *env ? std::string(env) : std::string(RN_TEST_TMP);
}

// Fresh, empty scratch directory unique to this process + name.
inline std::string tempDir(const std::string& name) {
  static std::atomic<int> counter{0};
  std::string d = rn::fs::join(tempRoot(), name + "-" + std::to_string(counter++));
  rn::fs::removeAll(d);
  rn::fs::createDirs(d);
  return d;
}

inline std::string writeTestRom(const std::string& dir, const std::string& name = "testrom.nes") {
  std::vector<uint8_t> rom = rn::buildTestRom();
  std::string p = rn::fs::join(dir, name);
  rn::fs::writeFileAtomic(p, rom.data(), rom.size());
  return p;
}

inline std::unique_ptr<rn::Session> newSession(rn::CoreKind kind, rn::CheckpointPolicy pol = {}) {
  rn::SessionOptions o;
  o.core = kind;
  o.checkpoints = pol;
  std::unique_ptr<rn::Session> s;
  rn::Status st = rn::Session::create(rn::buildTestRom(), "memory://testrom.nes", o, s);
  if (!st.ok()) return nullptr;
  return s;
}

// Straight replay of `recs` on a fresh core: per-frame video/audio hash + final state hash.
struct Replay {
  std::vector<uint64_t> video, audio, state;  // state[i] = machine hash after i+1 frames
  uint64_t finalState = 0;
};
inline Replay straightReplay(rn::CoreKind kind, const std::vector<rn::InputRecord>& recs, bool stateEachFrame = false,
                             bool frameHashes = true) {
  Replay r;
  auto core = rn::createCore(kind);
  std::vector<uint8_t> rom = rn::buildTestRom();
  core->loadROM(rom.data(), rom.size());
  for (auto& rec : recs) {
    core->stepRecord(rec.p1, rec.p2, rec.events, frameHashes);
    if (!frameHashes) continue;
    r.video.push_back(core->videoHash());
    size_t n;
    const int16_t* a = core->audio(&n);
    r.audio.push_back(rn::Session::audioHashOf(a, n));
    if (stateEachFrame) r.state.push_back(core->machineHash());
  }
  r.finalState = core->machineHash();
  return r;
}

// Records a script through the session in RECORD mode.
inline rn::Status recordScript(rn::Session& s, const std::vector<rn::InputRecord>& recs) {
  for (auto& r : recs) RN_TRY(s.step(r.p1, r.p2, r.events));
  return rn::Status::Ok();
}

}  // namespace rntest
