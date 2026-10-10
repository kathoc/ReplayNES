// showprog (fragment) from the fast path's half-float tube output (show.frag otherwise).
// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "common.glsl"
layout(std430, set = 0, binding = 0) readonly buffer B0 { uvec2 img[]; };
layout(push_constant) uniform ShowParams { vec4 dst; vec4 src; int ow; int oh; int tw; int th; vec4 ndc; } P;
vec3 tubeAt(int x, int y) { x = clamp(x, 0, P.ow - 1); y = clamp(y, 0, P.oh - 1); uvec2 q = img[y * P.ow + x]; return vec3(unpackHalf2x16(q.x), unpackHalf2x16(q.y).x); }
vec3 encodeSRGB(vec3 v) {
  v = max(vec3(0.0), v);
  vec3 s = mix(1.055 * pow(v, vec3(1.0 / 2.4)) - 0.055, 12.92 * v, lessThanEqual(v, vec3(0.0031308)));
  return min(s, vec3(1.0));
}
vec3 showSample(vec2 frag) {
  float u = (frag.x - P.dst.x) / P.dst.z * P.src.z + P.src.x - 0.5;
  float v = (frag.y - P.dst.y) / P.dst.w * P.src.w + P.src.y - 0.5;
  float x0 = floor(u), y0 = floor(v), fx = u - x0, fy = v - y0;
  int ix = int(x0), iy = int(y0);
  vec3 a = tubeAt(ix, iy), b = tubeAt(ix + 1, iy);
  vec3 c = tubeAt(ix, iy + 1), d = tubeAt(ix + 1, iy + 1);
  vec3 top = fx == 0.0 ? a : a * (1.0 - fx) + b * fx;
  vec3 bot = fx == 0.0 ? c : c * (1.0 - fx) + d * fx;
  return fy == 0.0 ? top : top * (1.0 - fy) + bot * fy;
}
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(encodeSRGB(showSample(gl_FragCoord.xy)), 1.0); }
