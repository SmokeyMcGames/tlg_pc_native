// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone native SPIR-V cache tests; no Vulkan headers, ICD or game files.
#include "../gpu/shim/tlg_native_spirv_registry.h"
#include <cassert>
#include <cstdint>
#include <vector>

static TlgNativeVk::NativeShaderKey Key(std::uint64_t guest) {
    return {
        .guest_id = guest,
        .game_binary_hash = 0x100abc,
        .code_hash = guest + 7,
        .specialization_hash = 11,
        .resource_layout_hash = 13,
        .compiler_revision = 17,
        .vk_vendor = 0x10de,
        .vk_device = 0x2684,
        .vk_driver = 595,
        .stage = TlgNativeVk::NativeShaderStage::Fragment,
    };
}
int main() {
    using namespace TlgNativeVk;
    const std::vector<std::uint32_t> spirv{0x07230203,0x00010000,1,2,3};
    const std::vector<std::uint32_t> bigger{0x07230203,0x00010000,1,2,3,4,5,6};
    NativeShaderRegistry cache(40);
    auto key=Key(0x123);
    assert(NativeShaderRegistry::IsCompleteKey(key));
    assert(!cache.Publish(key,spirv,false));
    assert(cache.ResidentBytes()==0);
    assert(cache.Publish(key,spirv,true));
    assert(cache.ResidentBytes()==spirv.size()*4);
    auto retained=cache.Find(key);
    assert(retained && retained->generation==1 && retained->spirv==spirv);
    auto alternate=key;
    alternate.vk_driver+=1;
    assert(!cache.Find(alternate)); // never reuse a different GPU driver
    alternate=key;
    alternate.specialization_hash+=1;
    assert(!cache.Find(alternate)); // never reuse a changed resource layout
    alternate=key;
    alternate.code_hash+=1;
    assert(!cache.Find(alternate));
    // Another shader consumes most of the budget. A failed replacement of
    // the first must leave its previous working entry intact.
    auto second=Key(0x234);
    assert(cache.Publish(second,spirv,true));
    assert(cache.ResidentBytes()==40);
    assert(!cache.Publish(key,bigger,true));
    assert(cache.Find(key)==retained);
    assert(cache.ResidentBytes()==40);
    cache.Retire(second.guest_id);
    assert(cache.Publish(key,bigger,true));
    auto newer=cache.Find(key);
    assert(newer && newer->generation==3 && newer->spirv==bigger);
    assert(retained->generation==1 && retained->spirv==spirv);
    assert(cache.ResidentBytes()==32);
    cache.Retire(key.guest_id);
    assert(cache.ResidentBytes()==0);
    assert(!cache.Find(key));
    assert(retained->spirv==spirv && newer->spirv==bigger);

    // The descriptor-enabled cache must preserve layouts, stage offsets
    // and retained lifetimes, and reject an incompatible fragment layout.
    NativeShaderRegistry layout_cache(2048);
    auto vs_key = Key(0x600);
    vs_key.stage = NativeShaderStage::Vertex;
    auto fs_key = Key(0x601);
    fs_key.stage = NativeShaderStage::Fragment;
    std::vector<NativeDescriptorBinding> vs_descriptors{
        {0, 1, NativeDescriptorKind::StorageBuffer},
        {1, 1, NativeDescriptorKind::SampledImage},
    };
    std::vector<NativeDescriptorBinding> fs_descriptors{
        {2, 1, NativeDescriptorKind::Sampler},
    };
    assert(!layout_cache.Publish(vs_key,spirv,true,fs_descriptors));
    assert(layout_cache.Publish(vs_key,spirv,true,vs_descriptors));
    assert(!layout_cache.Publish(fs_key,spirv,true,fs_descriptors,0));
    assert(layout_cache.Publish(fs_key,spirv,true,fs_descriptors,2));
    auto native_vs = layout_cache.Find(vs_key);
    auto native_fs = layout_cache.Find(fs_key);
    assert(native_vs && native_fs);
    assert(native_fs->first_binding == 2 &&
           native_fs->descriptors == fs_descriptors);
    std::vector<NativeDescriptorBinding> linked;
    assert(AssembleCachedGraphicsDescriptors(*native_vs,*native_fs,linked));
    assert(linked.size()==3 && linked.back().binding==2);
    const std::size_t layout_occupancy = spirv.size_bytes()*2 +
        (vs_descriptors.size()+fs_descriptors.size())*
            sizeof(NativeDescriptorBinding);
    assert(layout_cache.ResidentBytes()==layout_occupancy);
    auto failed_fs = *native_fs;
    failed_fs.first_binding = 1;
    assert(!AssembleCachedGraphicsDescriptors(*native_vs,failed_fs,linked));
    assert(linked.empty());
    failed_fs = *native_fs;
    failed_fs.descriptors[0].binding = 3;
    assert(!AssembleCachedGraphicsDescriptors(*native_vs,failed_fs,linked));
    assert(linked.empty());
    layout_cache.Retire(fs_key.guest_id);
    assert(native_fs->descriptors==fs_descriptors); // immutable old lifetime
    assert(layout_cache.ResidentBytes()==spirv.size_bytes() +
        vs_descriptors.size()*sizeof(NativeDescriptorBinding));

    auto invalid=Key(0x345);
    invalid.game_binary_hash=0;
    assert(!cache.Publish(invalid,spirv,true));
    const std::uint32_t invalid_magic[5]={0,1,2,3,4};
    assert(!cache.Publish(Key(0x444),invalid_magic,true));
    return 0;
}
