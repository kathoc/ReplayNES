// rf-webgl.mjs initCodes (+ M3-NOISE). The including shader declares `codes` (uint pairs of the
// 16-bit PPU codes), `phases`, `volts` and the RFParams push constants `P`.
// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md
#include "precise_math.glsl"

float gaussAt(int pos, int key) {
  uint k = uint(key);
  float u1 = (float(lowbias((uint(pos) * 2u) ^ k)) + 0.5) / 4294967296.0;
  float u2 = (float(lowbias((uint(pos) * 2u + 1u) ^ k)) + 0.5) / 4294967296.0;
  return sqrt(-2.0 * rn_log(u1)) * rn_cos(6.283185307179586 * u2);
}

uint codeAt(int i) { uint w = codes[i >> 1]; return (i & 1) != 0 ? (w >> 16u) : (w & 0xFFFFu); }

// initCodes: PPU codes -> composite voltage -> AM carrier (+ M3-NOISE) at sample `pos`.
float rfInitAt(int pos) {
  float v = 0.312;
  if (pos >= 0 && pos < 654720) {
    int row = pos / 2728; int n = pos % 2728; int phase = (n + phases[row]) % 12;
    if (n >= 36 && n < 2084) { int code = int(codeAt(row * 256 + (n - 36) / 8)); v = volts[phase * 512 + code]; }
    else if (n >= 2244 && n < 2444) v = 0.048;
    else if (n >= 2476 && n < 2596) v = ((8 + phase) % 12 < 6) ? 0.524 : 0.148;
  }
  float noise = (pos >= 0 && pos < 654720 && P.noiseSigma > 0.0) ? P.noiseSigma * gaussAt(pos, P.noiseKey) : 0.0;
  precise float r = (1.0 - P.scale * (v - 0.048)) * P.signalLevel + noise;
  return r;
}
// Bit-reversed rows (the reference FFT's input order).
float rfInitValue(int x, int block) { return rfInitAt(block * P.hop + P.delay - P.overlap + reverse12(x)); }

