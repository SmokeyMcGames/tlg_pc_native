// SPDX-License-Identifier: GPL-2.0-or-later
// Static native Vulkan test color shader. This does not translate game GCN.
#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(uv.x, uv.y, 0.25, 1.0);
}
