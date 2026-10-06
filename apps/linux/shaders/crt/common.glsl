// Shared helpers of the CRT compute shaders: GLSL ports of the MSL helpers in
// apps/macos/Sources/Core/CRT/CRTShaders.swift (themselves 1:1 with nesterm's WebGL2 shaders).
// Images are float4 buffers (vec4), row 0 = top, index y*width+x. Arithmetic is marked `precise`
// where the bit-identity checks (fast kernels == direct port) depend on it: no FMA contraction,
// like Metal's safe math mode.
// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md

vec3 glmix3(vec3 x, vec3 y, float a) { precise vec3 r = x * (1.0 - a) + y * a; return r; }
float glmix1(float x, float y, float a) { precise float r = x * (1.0 - a) + y * a; return r; }

int reverse12(int x) { int y = 0; for (int b = 0; b < 12; b++) { y = (y << 1) | (x & 1); x >>= 1; } return y; }
vec2 cmul(vec2 a, vec2 b) {
  precise float re = a.x * b.x - a.y * b.y;
  precise float im = a.x * b.y + a.y * b.x;
  return vec2(re, im);
}
uint lowbias(uint x) { x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu; x ^= x >> 16u; return x; }

#define TUBE_PARAMS                                                                                 \
  layout(push_constant) uniform TubeParams {                                                        \
    int width; int height; int ow; int oh; int xCount; int yCount; int gCount; int levels;          \
    float gain; float scatterFraction; float keep; int radius; int depth; int extra; int pad1; int pad2; \
  } P;

#define RF_PARAMS layout(push_constant) uniform RFParams { int hop; int overlap; int delay; int blocks; float scale; float signalLevel; float noiseSigma; int noiseKey; } P;
