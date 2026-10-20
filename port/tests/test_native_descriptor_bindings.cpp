// SPDX-License-Identifier: GPL-2.0-or-later
// Checks exact guest-object/descriptor shape without Vulkan or game assets.
#include "../gpu/shim/tlg_native_descriptor_bindings.h"
#include <cassert>
#include <vector>

int main() {
    using namespace TlgNativeVk;
    const std::vector<NativeDescriptorBinding> shader = {
        {0, 2, NativeDescriptorKind::SampledImage},
        {2, 1, NativeDescriptorKind::Sampler},
    };
    const std::vector<NativeGuestDescriptorBinding> good = {
        {0, NativeDescriptorKind::SampledImage, {{11, 1}, {12, 3}}},
        {2, NativeDescriptorKind::Sampler, {{17, 5}}},
    };
    assert(ValidateNativeGuestDescriptors(shader, good));
    assert(ValidateNativeGuestDescriptors({}, {}));
    assert(!ValidateNativeGuestDescriptors(shader, {}));
    auto bad = good;
    bad[0].kind = NativeDescriptorKind::StorageImage;
    assert(!ValidateNativeGuestDescriptors(shader, bad));
    bad = good;
    bad[1].binding = 1;
    assert(!ValidateNativeGuestDescriptors(shader, bad));
    bad = good;
    bad[0].resources.pop_back();
    assert(!ValidateNativeGuestDescriptors(shader, bad));
    bad = good;
    bad[0].resources.push_back({99, 1});
    assert(!ValidateNativeGuestDescriptors(shader, bad));
    bad = good;
    bad[0].resources[0].guest_id = 0;
    assert(!ValidateNativeGuestDescriptors(shader, bad));
    bad = good;
    bad[0].resources[0].generation = 0;
    assert(!ValidateNativeGuestDescriptors(shader, bad));
    auto mismatch = shader;
    mismatch[1].binding = 1;
    assert(!ValidateNativeGuestDescriptors(mismatch, good));
    mismatch = shader;
    mismatch[0].descriptor_count = 3;
    assert(!ValidateNativeGuestDescriptors(mismatch, good));
    std::vector<NativeDescriptorBinding> unsupported = {
        {0, 1, NativeDescriptorKind::StorageBuffer},
    };
    // StorageBuffer shape itself is legal, but the current Vulkan recorder
    // deliberately rejects it until an independent native buffer owner exists.
    assert(ValidateNativeGuestDescriptors(
        unsupported, std::vector<NativeGuestDescriptorBinding>{
            {0, NativeDescriptorKind::StorageBuffer, {{7, 2}}}}));
    return 0;
}
