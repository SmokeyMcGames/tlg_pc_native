// SPDX-License-Identifier: GPL-2.0-or-later
// Native Vulkan graphics-object registry, binding and barrier lifetime tests.
// Builds with a C++23 compiler; no Vulkan driver, GPU, assets or PS4 files.
#include "../gpu/shim/tlg_native_vk_resources.h"
#include <cassert>

static TlgJsNativeDrawCandidate GameDraw() {
    TlgJsNativeCommand event{};
    event.ready = 1;
    event.kind = TLG_JS_NATIVE_INDEXED;
    event.context = 0x111;
    event.arg0 = 64; // index count
    event.arg1 = 16; // index offset
    event.arg2 = 17; // vertex base
    TlgJsNativeDrawCandidate game{};
    assert(tlg_js_native_decode_draw(&event,&game));
    return game;
}
int main() {
    using namespace TlgNativeVk;
    ResourceRegistry gpu;
    NativeBoundDraw output{};
    const auto game = GameDraw();
    assert(!gpu.TryResolve(game,output));
    assert(!gpu.RegisterPipeline(0,33));
    assert(!gpu.RegisterIndexBuffer(0x200,0,512));
    assert(!gpu.RegisterIndexBuffer(0x200,555,0));
    assert(gpu.RegisterPipeline(0x100,33));
    assert(gpu.RegisterComputePipeline(0x101,44));
    assert(!gpu.RegisterComputePipeline(0x100,99)); // graphics ID collision
    assert(!gpu.RegisterPipeline(0x101,99)); // compute ID collision
    std::uint64_t compute_handle=0;
    assert(gpu.LookupComputePipeline(0x101,compute_handle));
    assert(compute_handle==44);
    assert(!gpu.LookupComputePipeline(0x100,compute_handle));
    assert(!gpu.Bind(0x111,0x101,0x200,0x300,0,17));
    assert(gpu.RegisterIndexBuffer(0x200,555,512));
    assert(gpu.RegisterTargetSet(0x300,777));
    // A real non-indexed Js Draw only needs pipeline, framebuffer and
    // independently verified vertex-fetch state (not an index buffer).
    const TlgJsNativeCommand ordinary_event{
        .ready=77, .kind=TLG_JS_NATIVE_DRAW, .context=0x333,
        .arg0=6, .arg1=4,
    };
    TlgJsNativeNonIndexedCandidate ordinary{};
    assert(tlg_js_native_decode_nonindexed(&ordinary_event,&ordinary));
    assert(!gpu.BindNonIndexed(0x333,0x100,0x300,false));
    assert(gpu.BindNonIndexed(0x333,0x100,0x300,true));
    assert(!gpu.TryResolveNonIndexed(ordinary,output)); // missing barrier
    assert(gpu.AcknowledgeBarriers(0x333,1));
    assert(gpu.TryResolveNonIndexed(ordinary,output));
    assert(!output.command.indexed && output.command.vertex_or_index_count==6);
    assert(output.command.first_vertex==4 && output.index_buffer==0);
    assert(output.pipeline==33 && output.target_set==777);
    gpu.Unbind(0x333);
    assert(!gpu.TryResolveNonIndexed(ordinary,output));
    // A compute VkPipeline must never satisfy the graphics draw bind.
    assert(!gpu.BindNonIndexed(0x333,0x101,0x300,true));
    assert(!gpu.Bind(0x111,0x100,0x222,0x300,0,17));
    assert(gpu.Bind(0x111,0x100,0x200,0x300,0,17));
    assert(!gpu.TryResolve(game,output)); // barrier must be recorded separately
    assert(gpu.AcknowledgeBarriers(0x111,4));
    assert(gpu.TryResolve(game,output));
    assert(output.pipeline==33 && output.index_buffer==555 && output.target_set==777);
    assert(output.command.vertex_or_index_count==64);
    assert(output.command.first_vertex==17 && output.command.first_index==16);
    gpu.NoteBufferWrite(0x200,5);
    assert(!gpu.TryResolve(game,output)); // unresolved upload
    assert(gpu.AcknowledgeBarriers(0x111,5));
    assert(gpu.TryResolve(game,output));
    gpu.NoteBufferWrite(0x200,4); // obsolete write ticket does not rewind
    assert(gpu.TryResolve(game,output));
    gpu.RegisterIndexBuffer(0x200,556,128); // new incarnation invalidates snapshot
    assert(!gpu.TryResolve(game,output));
    assert(gpu.Bind(0x111,0x100,0x200,0x300,10,17));
    assert(gpu.AcknowledgeBarriers(0x111,8));
    assert(gpu.TryResolve(game,output));
    assert(output.command.first_index==6); // bound 10 indices into guest buffer
    assert(output.index_buffer==556);
    gpu.UnregisterTargetSet(0x300);
    assert(!gpu.TryResolve(game,output));
    assert(gpu.RegisterTargetSet(0x300,778));
    assert(gpu.Bind(0x111,0x100,0x200,0x300,0,17));
    assert(gpu.AcknowledgeBarriers(0x111,9));
    assert(gpu.TryResolve(game,output));
    assert(output.target_set==778);
    auto other=game;
    other.context=0x112;
    assert(!gpu.TryResolve(other,output));
    other.context=0x111;
    other.vertex_base=0; // mismatched recovered guest shader constants
    assert(!gpu.TryResolve(other,output));
    gpu.Unbind(0x111);
    assert(!gpu.TryResolve(game,output));

    // Native sampled images are explicitly unavailable until a GPU
    // transfer + shader-read layout barrier has been recorded.
    ResourceRegistry::StorageBufferIdentity storage{};
    assert(!gpu.TryResolveStorageBuffer(0xb01,storage));
    assert(!gpu.RegisterStorageBuffer(0,100,64));
    assert(!gpu.RegisterStorageBuffer(0xb01,100,0));
    assert(gpu.RegisterStorageBuffer(0xb01,100,64));
    assert(!gpu.TryResolveStorageBuffer(0xb01,storage));
    assert(!gpu.MarkStorageBufferReady(0xb01,999));
    assert(gpu.MarkStorageBufferReady(0xb01,100));
    assert(gpu.TryResolveStorageBuffer(0xb01,storage));
    assert(storage.vk_handle==100 && storage.capacity_bytes==64);
    const auto first_storage_generation=storage.generation;
    assert(gpu.RegisterStorageBuffer(0xb01,200,128));
    assert(!gpu.TryResolveStorageBuffer(0xb01,storage));
    assert(gpu.MarkStorageBufferReady(0xb01,200));
    assert(gpu.TryResolveStorageBuffer(0xb01,storage));
    assert(storage.vk_handle==200 && storage.capacity_bytes==128 &&
           storage.generation>first_storage_generation);
    gpu.UnregisterStorageBuffer(0xb01);
    assert(!gpu.TryResolveStorageBuffer(0xb01,storage));

    ResourceRegistry::SampledBinding sampled{};
    assert(!gpu.TryResolveSampledBinding(0x901,0x902,sampled));
    ResourceRegistry::SampledIdentity native_identity{};
    assert(!gpu.TryResolveSampledImage(0x901,native_identity));
    assert(!gpu.TryResolveSampler(0x902,native_identity));
    assert(!gpu.RegisterSampledImage(0,101));
    assert(!gpu.RegisterSampler(0,202));
    assert(gpu.RegisterSampledImage(0x901,101));
    assert(gpu.RegisterSampler(0x902,202));
    assert(!gpu.TryResolveSampledBinding(0x901,0x902,sampled));
    assert(!gpu.TryResolveSampledImage(0x901,native_identity));
    assert(gpu.TryResolveSampler(0x902,native_identity));
    assert(native_identity.vk_handle==202 && native_identity.generation!=0);
    assert(!gpu.MarkSampledImageReady(0x901,999)); // different image identity
    assert(gpu.MarkSampledImageReady(0x901,101));
    assert(gpu.TryResolveSampledBinding(0x901,0x902,sampled));
    assert(sampled.image_view==101 && sampled.sampler==202);
    const auto first_image_generation=sampled.image_generation;
    const auto first_sampler_generation=sampled.sampler_generation;
    assert(gpu.TryResolveSampledImage(0x901,native_identity));
    assert(native_identity.vk_handle==101 &&
           native_identity.generation==first_image_generation);
    assert(gpu.RegisterSampledImage(0x901,303)); // old view invalidated
    assert(!gpu.TryResolveSampledBinding(0x901,0x902,sampled));
    assert(!gpu.TryResolveSampledImage(0x901,native_identity));
    assert(!gpu.MarkSampledImageReady(0x901,101));
    assert(gpu.MarkSampledImageReady(0x901,303));
    assert(gpu.TryResolveSampledBinding(0x901,0x902,sampled));
    assert(sampled.image_view==303 && sampled.image_generation>first_image_generation);
    assert(sampled.sampler_generation==first_sampler_generation);
    assert(gpu.RegisterSampler(0x902,404));
    assert(gpu.TryResolveSampledBinding(0x901,0x902,sampled));
    assert(sampled.sampler==404 && sampled.sampler_generation>first_sampler_generation);
    assert(gpu.TryResolveSampler(0x902,native_identity));
    assert(native_identity.vk_handle==404 &&
           native_identity.generation==sampled.sampler_generation);
    gpu.UnregisterSampledImage(0x901);
    assert(!gpu.TryResolveSampledImage(0x901,native_identity));
    assert(!gpu.TryResolveSampledBinding(0x901,0x902,sampled));
    gpu.UnregisterSampler(0x902);
    assert(!gpu.TryResolveSampler(0x902,native_identity));
    return 0;
}
