// The blurred backdrop behind the game picture (docs/design/BACKDROP.md): the picture's mip chain read
// at a coarse level, a 3x3 tent of it, dimmed. SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC { layout(offset = 32) vec4 p; } pc;  // lod, dim, texel u, texel v
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
  vec3 c = vec3(0.0);
  float w = 0.0;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      float k = (2.0 - abs(float(x))) * (2.0 - abs(float(y)));
      c += k * textureLod(tex, vUV + vec2(x, y) * pc.p.zw, pc.p.x).rgb;
      w += k;
    }
  outColor = vec4(c / w * pc.p.y, 1.0);
}
