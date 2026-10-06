// Game picture: one quad covering the viewport (the destination rectangle); uv = visible source
// rectangle (overscan crop). SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(push_constant) uniform PC { vec4 uv; } pc;  // u0, v0, u1, v1
layout(location = 0) out vec2 vUV;
void main() {
  vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
  vUV = mix(pc.uv.xy, pc.uv.zw, p);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
