// Nearest-neighbour sample of the 256x240 BGRA frame (the sampler uses NEAREST filtering).
// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(texture(tex, vUV).rgb, 1.0); }
