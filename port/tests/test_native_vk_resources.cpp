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
    return 0;
}
