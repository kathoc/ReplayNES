// ReplayNES generated test ROM (own code, hand-assembled 6502; no third-party ROM).
// NROM-128 (mapper 0), 16 KiB PRG + 8 KiB CHR. Every NMI it: reads both pads, steps a
// 16-bit Galois LFSR in RAM and mixes the pad bits into it, writes LFSR-derived tiles to the
// nametable, changes palette entries (video) and pulse/triangle/noise pitch (audio).
// RESET does not clear RAM, so soft-reset timing changes subsequent output deterministically.
#pragma once
#include <cstdint>
#include <vector>

namespace rn {
std::vector<uint8_t> buildTestRom();
// Zero-page layout (for tests / documentation).
enum TestRomZp : uint8_t { ZP_PAD1 = 0, ZP_PAD2 = 1, ZP_SEED_LO = 2, ZP_SEED_HI = 3, ZP_FC = 4, ZP_FC2 = 5, ZP_RESETS = 7 };
}  // namespace rn
