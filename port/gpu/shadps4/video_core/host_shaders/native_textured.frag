// SPDX-License-Identifier: GPL-2.0-or-later
// Native Vulkan offscreen sampled-texture verification (not game PSSL).
#version 450
layout(location = 0) in vec2 uv;
layout(set = 0, binding = 0) uniform texture2D native_image;
layout(set = 0, binding = 1) uniform sampler native_sampler;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(sampler2D(native_image, native_sampler), uv);
}
