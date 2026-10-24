// Golden replay digests through the public C API: Nestopia + the ReplayNES Test Cartridge with a
// fixed input script (soft reset, power cycle, a rewind-and-branch, save and reopen). The video and
// audio of every frame are hashed and compared with tests/fixtures/golden/nestopia_testcart.txt, so a
// refactoring of the engine that changes any emulated frame fails here even when both runs of a
// relative determinism test would agree. Projects move between macOS, Linux and Windows, so the
// golden values are the same on every platform.
//
// RN_GOLDEN_UPDATE=1 rewrites the golden file (only when a change of the output is intended).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "replaynes/replaynes.h"
#include "support/TestUtil.h"
#include "support/rn_test.h"

using namespace rntest;

namespace {

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
  auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

struct FrameHash {
  uint64_t video, audio;
  bool operator==(const FrameHash& o) const { return video == o.video && audio == o.audio; }
};

FrameHash hashFrame(const rn_session* s) {
  size_t n = 0;
  const int16_t* pcm = rn_audio(s, &n);
  return {fnv(rn_video(s), size_t(RN_VIDEO_WIDTH) * RN_VIDEO_HEIGHT * 4), fnv(pcm, n * sizeof(int16_t), fnv(&n, sizeof n))};
}

// Buttons change every 6 frames; the Test Cartridge reads both pads on every screen.
uint8_t pad(uint64_t f, uint64_t salt) {
  uint64_t x = (f / 6 + 1) * 0x9E3779B97F4A7C15ull ^ salt;
  x ^= x >> 31;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 29;
  return uint8_t(x);
}

// Running digest of a run, written out every 100 frames: "<label> <frame> <video> <audio>".
struct Digest {
  std::string label;
  uint64_t v = 0, a = 0;
  std::ostringstream out;
  explicit Digest(std::string l) : label(std::move(l)) {}
  void add(uint64_t frame, const FrameHash& h) {
    v = fnv(&h.video, sizeof h.video, v);
    a = fnv(&h.audio, sizeof h.audio, a);
    if (frame % 100 == 99) {
      char line[96];
      snprintf(line, sizeof line, "%s %llu %016llx %016llx\n", label.c_str(), (unsigned long long)frame,
               (unsigned long long)v, (unsigned long long)a);
      out << line;
    }
  }
};

std::vector<FrameHash> replayAll(rn_session* s, Digest& d) {
  REQUIRE(rn_set_mode(s, RN_MODE_REPLAY) == RN_OK);
  REQUIRE(rn_seek(s, 0) == RN_OK);
  std::vector<FrameHash> hashes;
  for (;;) {
    rn_step_info info{};
    uint64_t before = rn_frame(s);
    REQUIRE(rn_step(s, 0, 0, 0, &info) == RN_OK);
    if (info.frame == before) break;  // at the take end nothing is emulated
    hashes.push_back(hashFrame(s));
    d.add(before, hashes.back());
    if (info.end_of_take) break;
  }
  return hashes;
}

std::string goldenPath() { return std::string(RN_GOLDEN_DIR) + "/nestopia_testcart.txt"; }

}  // namespace

TEST_CASE("golden: Nestopia + Test Cartridge record, branch, replay, save and reopen") {
  std::string dir = tempDir("golden");
  std::string rom = dir + "/testcart.nes";
  REQUIRE(rn_write_test_cartridge(rom.c_str()) == RN_OK);

  rn_session_options opt;
  rn_session_options_init(&opt);
  opt.core = RN_CORE_NESTOPIA;
  rn_session* s = nullptr;
  REQUIRE(rn_session_new(rom.c_str(), nullptr, &opt, &s) == RN_OK);
  REQUIRE(rn_get_mode(s) == RN_MODE_RECORD);

  // A: 3000 frames from power-on with a soft reset at 1000 and a power cycle at 2000.
  Digest a("record");
  std::vector<FrameHash> recorded;
  for (uint64_t f = 0; f < 3000; f++) {
    uint8_t ev = f == 1000 ? RN_EV_SOFT_RESET : f == 2000 ? RN_EV_POWER_CYCLE : 0;
    REQUIRE(rn_step(s, pad(f, 1), pad(f, 2), ev, nullptr) == RN_OK);
    recorded.push_back(hashFrame(s));
    a.add(f, recorded.back());
  }

  // B: rewind to 1500 and record 500 frames with other input: a new take of 2000 frames.
  REQUIRE(rn_seek(s, 1500) == RN_OK);
  Digest b("branch");
  for (uint64_t f = 1500; f < 2000; f++) {
    REQUIRE(rn_step(s, pad(f, 3), pad(f, 4), 0, nullptr) == RN_OK);
    recorded[f] = hashFrame(s);
    b.add(f, recorded[f]);
  }
  recorded.resize(2000);
  REQUIRE(rn_take_length(s) == 2000);

  // Replaying the take reproduces every recorded frame (independent of the golden file).
  Digest r("replay");
  std::vector<FrameHash> replayed = replayAll(s, r);
  REQUIRE(replayed.size() == recorded.size());
  for (size_t f = 0; f < replayed.size(); f++) {
    if (!(replayed[f] == recorded[f])) FAIL("replay differs from the recording at frame " + std::to_string(f));
  }

  // C: save, close, reopen: the reopened take replays the same frames.
  std::string project = dir + "/golden.nesrec";
  REQUIRE(rn_session_save_as(s, project.c_str()) == RN_OK);
  rn_session_close(s);
  s = nullptr;
  REQUIRE(rn_session_open(project.c_str(), rom.c_str(), &opt, &s) == RN_OK);
  Digest c("reopen");
  std::vector<FrameHash> reopened = replayAll(s, c);
  rn_session_close(s);
  REQUIRE(reopened.size() == recorded.size());
  for (size_t f = 0; f < reopened.size(); f++) {
    if (!(reopened[f] == recorded[f])) FAIL("reopened replay differs at frame " + std::to_string(f));
  }

  // D: a project saved by an earlier build (tests/fixtures/golden/project-v2.nesrec, file format 2, input
  // format 1) still opens and replays the same frames: newer builds must keep reading old projects.
  std::string fixture = std::string(RN_GOLDEN_DIR) + "/project-v2.nesrec";
  const char* update = std::getenv("RN_GOLDEN_UPDATE");
  bool updating = update && *update == '1';
  if (updating && !rn::fs::exists(fixture)) {
    std::filesystem::copy(project, fixture, std::filesystem::copy_options::recursive);
    // The ROM's path on this machine must not end up in the repository: keep only its name (the
    // test opens the fixture with a ROM override anyway).
    for (const char* name : {"manifest.json", "timeline/index.json"}) {
      std::string path = fixture + "/" + name, text;
      {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
      }
      for (size_t at; (at = text.find(rom)) != std::string::npos;) text.replace(at, rom.size(), "testcart.nes");
      std::ofstream(path, std::ios::binary) << text;
    }
    MESSAGE("fixture project written: " + fixture);
  }
  REQUIRE(rn_session_open(fixture.c_str(), rom.c_str(), &opt, &s) == RN_OK);
  Digest d("fixture");
  std::vector<FrameHash> old = replayAll(s, d);
  rn_session_close(s);
  REQUIRE(old.size() == recorded.size());
  for (size_t f = 0; f < old.size(); f++) {
    if (!(old[f] == recorded[f])) FAIL("fixture project replay differs at frame " + std::to_string(f));
  }

  std::string actual = a.out.str() + b.out.str() + r.out.str() + c.out.str() + d.out.str();
  if (updating) {
    std::ofstream(goldenPath(), std::ios::binary) << actual;
    MESSAGE("golden file rewritten: " + goldenPath());
    return;
  }
  std::ifstream in(goldenPath(), std::ios::binary);
  REQUIRE(in.good());
  std::stringstream expected;
  expected << in.rdbuf();
  if (expected.str() != actual) {
    std::istringstream e(expected.str()), g(actual);
    std::string el, gl;
    while (std::getline(e, el) && std::getline(g, gl)) {
      if (el != gl) FAIL("first golden mismatch: expected '" + el + "', got '" + gl + "'");
    }
    FAIL("golden output length differs");
  }
}
