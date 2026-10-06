// showprog (vertex): quad over the destination rectangle (ndc = x0, y0, w, h in clip space).
// SPDX-License-Identifier: GPL-2.0-or-later (port); original MIT, see THIRD_PARTY_NOTICES.md
#version 450
#extension GL_GOOGLE_include_directive : enable
layout(push_constant) uniform ShowParams { vec4 dst; vec4 src; int ow; int oh; int tw; int th; vec4 ndc; } P;
void main() {
  vec2 c = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
  gl_Position = vec4(P.ndc.x + c.x * P.ndc.z, P.ndc.y + c.y * P.ndc.w, 0.0, 1.0);
}
