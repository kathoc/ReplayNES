#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace rntest {
struct VolumeCase {
  const char* name;
  uint8_t channels;
  uint8_t pulseVolume;
};

inline constexpr VolumeCase kVolumeCases[] = {
    {"pulse1-15", 1, 15}, {"pulse1-7", 1, 7},
    {"pulse2-15", 2, 15}, {"pulse2-7", 2, 7},
    {"triangle", 4, 15}, {"noise", 8, 15}, {"dmc", 16, 15},
    {"pulse-pair", 3, 15}, {"mixed", 31, 15}};

// Self-contained mapper-0 cartridge. No commercial ROM or assembler is needed.
// Inactive triangle is deliberately left at power-on: differing held DAC levels
// in the two cores are part of the behaviour being compared, not normalised away.
inline std::vector<uint8_t> volumeRom(const VolumeCase& test) {
  std::vector<uint8_t> rom(16 + 32768 + 8192, 0);
  rom[0] = 'N'; rom[1] = 'E'; rom[2] = 'S'; rom[3] = 0x1a;
  rom[4] = 2; rom[5] = 1;
  std::vector<uint8_t> code{0x78, 0xd8, 0xa2, 0xff, 0x9a}; // SEI; CLD; LDX #$ff; TXS
  const auto write = [&](uint16_t address, uint8_t value) {
    code.insert(code.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  write(0x2000, 0); write(0x2001, 0); write(0x4015, 0); write(0x4017, 0x40);
  // Initialise every audible register explicitly; length counters are loaded
  // only for enabled channels. Constant envelopes and halt flags sustain tones.
  for (uint16_t reg = 0x4000; reg <= 0x4013; ++reg) write(reg, 0);
  write(0x4015, test.channels & 15);
  if (test.channels & 1) {
    write(0x4000, uint8_t(0xb0 | test.pulseVolume)); // 50% duty, halt, constant volume
    write(0x4002, 253); write(0x4003, 0x08);       // ~440 Hz
  }
  if (test.channels & 2) {
    write(0x4004, uint8_t(0xb0 | test.pulseVolume));
    write(0x4006, 211); write(0x4007, 0x08);       // ~528 Hz
  }
  if (test.channels & 4) {
    // ~370 Hz. Not 440 Hz: at pulse1's frequency the mixed RMS measured the
    // two cores' start-up phase (constructive vs cancelling sum), not loudness.
    // Periods 32*151 and 16*254 share harmonics only above Nyquist.
    write(0x4008, 0xff); write(0x400a, 150); write(0x400b, 0x08);
  }
  if (test.channels & 8) {
    write(0x400c, 0x3f); write(0x400e, 10); write(0x400f, 0x08); // long LFSR, 380 cycles
  }
  if (test.channels & 16) {
    write(0x4010, 0x4f); // loop, no IRQ, NTSC period 54
    write(0x4011, 64); write(0x4012, 0); write(0x4013, 1); // $c000, 17 bytes
    write(0x4015, test.channels);
  }
  const uint16_t idle = uint16_t(0x8000 + code.size());
  code.insert(code.end(), {0x4c, uint8_t(idle), uint8_t(idle >> 8)});
  std::copy(code.begin(), code.end(), rom.begin() + 16);
  // Two equal down/up ramps followed by four balanced bit pairs. DAC stays
  // between 0 and 66 (the final $55 first adds 2 at level 64), so neither
  // saturation nor baseline drift shapes the tone.
  for (unsigned i = 0; i < 16; ++i) rom[16 + 0x4000 + i] = (i % 8 < 4) ? 0x00 : 0xff;
  rom[16 + 0x4010] = 0x55;
  for (unsigned vector : {0x7ffa, 0x7ffc, 0x7ffe}) {
    rom[16 + vector] = 0; rom[17 + vector] = 0x80;
  }
  return rom;
}
} // namespace rntest
