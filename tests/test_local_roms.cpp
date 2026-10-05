// Local-only checks against the developer's own ROMs in <repo>/roms (gitignored, never committed).
// Skipped when the directory is absent or empty, so CI and other machines are unaffected.
// For every *.nes there: the game boots to a non-blank picture, and machine state / video / audio
// stay bit-identical after a mid-run savestate load (with resets in the input script).
#include <algorithm>
#include <cctype>

#include "support/TestUtil.h"
#include "support/rn_test.h"

using namespace rn;
using namespace rntest;

namespace {
std::vector<std::string> localRoms() {
  std::vector<std::string> names, out;
  if (!fs::listDir(RN_LOCAL_ROMS, names).ok()) return out;
  for (auto& n : names) {
    std::string lower = n;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".nes") == 0) out.push_back(std::string(RN_LOCAL_ROMS) + "/" + n);
  }
  return out;
}
}  // namespace

TEST_CASE("local roms: boot to a picture and survive mid-run savestates") {
  auto roms = localRoms();
  if (roms.empty()) {
    MESSAGE(std::string("no ROMs in ") + RN_LOCAL_ROMS + " - skipped");
    return;
  }
  for (auto& path : roms) {
    std::vector<uint8_t> rom;
    REQUIRE(fs::readFile(path, rom).ok());
    MESSAGE(path);

    auto core = createCore(CoreKind::Nestopia);
    REQUIRE(core->loadROM(rom.data(), rom.size()).ok());
    size_t distinct = 0;
    for (int f = 0; f < 1200 && distinct < 4; ++f) {
      REQUIRE(core->stepFrame(0, 0, true).ok());
      if (f % 60 != 59) continue;
      std::vector<uint32_t> px(core->video(), core->video() + kVideoWidth * kVideoHeight);
      std::sort(px.begin(), px.end());
      distinct = size_t(std::unique(px.begin(), px.end()) - px.begin());
    }
    CHECK(distinct >= 4);  // a picture within 1200 frames (not a blank screen)

    // Gradius II (VRC4) diverged ~60 frames after a load at 600 before Nestopia patch 5.
    for (uint64_t mid : {600ull, 2501ull}) {
      auto script = DeterminismHarness::randomScript(4000, 11 + mid, 1700, 6);
      HarnessConfig cfg;
      cfg.runs = 1;
      cfg.stateEvery = 20;
      cfg.midFrame = mid;
      HarnessReport rep;
      REQUIRE(DeterminismHarness::run(cfg, rom, script, rep).ok());
      if (rep.mid.found) MESSAGE("mid@" + std::to_string(mid) + ": " + rep.mid.detail);
      CHECK_FALSE(rep.mid.found);
    }
  }
}
