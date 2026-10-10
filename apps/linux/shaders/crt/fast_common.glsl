// Fast display path (CRTRenderer.Quality.fast / CrtQuality::fast): GLSL twins of the MSL fast
// kernels in apps/macos/Sources/Core/CRT/CRTShaders.swift (`fast`), same structure and order.
// Not bit-identical to the reference kernels (no `precise`); bounded against them by the
// conformance harness (fastVsReference). docs/CRT_PORT.md "Fast path".
// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md

vec2 cmulF(vec2 a, vec2 b) { return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
// Half-float RGBA stored as two uints (packHalf2x16), the layout of Metal's half4.
vec4 unpackH4(uvec2 h) { return vec4(unpackHalf2x16(h.x), unpackHalf2x16(h.y)); }
uvec2 packH4(vec4 v) { return uvec2(packHalf2x16(v.xy), packHalf2x16(v.zw)); }

// 64 bytes of push constants, the same layout as the MSL FastTubeParams.
#define FAST_TUBE_PARAMS                                                                         \
  layout(push_constant) uniform FastTubeParams {                                                 \
    int height; int ow; int oh; int xCount; int gCount; int hStride; float gain; float keep;     \
    vec4 amb; vec4 light;                                                                        \
  } P;
