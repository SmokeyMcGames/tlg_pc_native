// SPDX-License-Identifier: GPL-2.0-or-later
// Strict descriptor reflection matching; standalone, needs no Vulkan ICD.
#include "../gpu/shim/tlg_native_descriptor_layout.h"
#include <cassert>
#include <vector>
using namespace TlgNativeVk;

int main() {
    std::vector<NativeDescriptorBinding> vs;
    assert(AppendNativeDescriptor(vs, 0, NativeDescriptorKind::StorageBuffer));
    assert(AppendNativeDescriptor(vs, 0, NativeDescriptorKind::SampledImage, 3));
    assert(AppendNativeDescriptor(vs, 0, NativeDescriptorKind::Sampler));
    assert(vs.size() == 3 && vs[0].binding == 0 && vs[1].binding == 1 &&
           vs[2].binding == 4 && ValidNativeDescriptorLayout(vs));
    auto wrong = vs;
    wrong[1].binding = 2;
    assert(!ValidNativeDescriptorLayout(wrong));
    assert(!MatchNativeDescriptorLayout(wrong, vs));
    wrong = vs;
    wrong[1].kind = NativeDescriptorKind::StorageImage;
    assert(!MatchNativeDescriptorLayout(wrong, vs));
    wrong = vs;
    wrong[1].descriptor_count = 2;
    assert(!MatchNativeDescriptorLayout(wrong, vs));
    assert(MatchNativeDescriptorLayout(vs, vs));

    // The fragment shader uses absolute binding numbers after the vertex stage.
    std::vector<NativeDescriptorBinding> fs;
    assert(AppendNativeDescriptor(fs, 5, NativeDescriptorKind::SampledImage));
    assert(AppendNativeDescriptor(fs, 5, NativeDescriptorKind::Sampler));
    assert(fs[0].binding == 5 && fs[1].binding == 6);
    assert(!ValidNativeDescriptorLayout(fs));
    assert(ValidNativeDescriptorLayout(fs, 5));
    std::vector<NativeDescriptorBinding> pipeline = vs;
    pipeline.insert(pipeline.end(), fs.begin(), fs.end());
    assert(ValidNativeDescriptorLayout(pipeline));

    std::vector<NativeDescriptorBinding> zero;
    assert(!AppendNativeDescriptor(zero, 0, NativeDescriptorKind::Sampler, 0));
    assert(!AppendNativeDescriptor(zero, 0, static_cast<NativeDescriptorKind>(99)));
    assert(zero.empty());
    assert(AppendNativeDescriptor(zero, 63, NativeDescriptorKind::Sampler));
    assert(!AppendNativeDescriptor(zero, 63, NativeDescriptorKind::Sampler));
    assert(!AppendNativeDescriptor(zero, 0, NativeDescriptorKind::Sampler));
    assert(!AppendNativeDescriptor(zero, 63, NativeDescriptorKind::SampledImage, 2));
    assert(ValidNativeDescriptorLayout(zero, 63));
    assert(!ValidNativeDescriptorLayout(zero, 64));
    assert(ValidNativeDescriptorLayout({}));
    assert(!ValidNativeDescriptorLayout({}, 65));
    return 0;
}
