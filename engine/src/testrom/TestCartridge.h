// ReplayNES Test Cartridge: a test / demo program for the NES written for this project
// (tools/testcart: 6502 source, own tiles and font, CC0-1.0). NROM-128, 16 KiB PRG + 8 KiB CHR.
// Screens: palette chart (all 64 colours), colour bars, sprites (sprite-0 hit, 8 per line),
// scrolling (incl. split), both controllers, rapid-fire meter (presses / edges per second, hold /
// gap, deviation, turbo check), sound test. The ROM image is generated source
// (TestCartridge.inc, checked against the assembly by tools/testcart/build.py --check).
#pragma once
#include <cstdint>
#include <vector>

namespace rn {
std::vector<uint8_t> buildTestCartridge();

// CPU RAM addresses / screen numbers of the cartridge (for tests).
namespace testcart {
#include "testrom/TestCartridgeSyms.inc"
}  // namespace testcart
}  // namespace rn
