// SPDX-License-Identifier: GPL-2.0-or-later
// Test the complete semantic-argument -> verified resources -> Vulkan draw
// conversion. No game files, Vulkan loader, pipeline driver or GPU required.
#include "../gpu/shim/tlg_js_native_translate.h"
#include <cassert>
#include <cstdint>
struct FakeCommandBuffer {
    unsigned* calls;
    std::uint32_t* args;
    void bindPipeline(int, int pipeline) {
        ++calls[0]; args[0] = pipeline;
    }
    void drawIndexed(std::uint32_t count, std::uint32_t instances, std::uint32_t first,
                     std::int32_t vertex_base, std::uint32_t first_instance) {
        ++calls[1]; args[1]=count; args[2]=instances; args[3]=first;
        args[4]=std::uint32_t(vertex_base); args[5]=first_instance;
    }
    void draw(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
        assert(false && "Native Js indexed draw should not become Draw()");
    }
};
int main() {
    TlgJsNativeCommand record{};
    record.ready=1; record.kind=TLG_JS_NATIVE_INDEXED; record.context=0x1000;
    record.arg0=220; record.arg1=35; record.arg2=77;
    TlgJsNativeDrawCandidate source{};
    assert(tlg_js_native_decode_draw(&record,&source));
    TlgNativeVk::ResolvedJsGraphics binding{};
    binding.guest_context=0x1000;
    binding.shader_generation=4;
    binding.resource_generation=6;
    binding.index_capacity=1024;
    binding.vertex_base=77;
    binding.first_instance=0;
    binding.pipeline_bound=binding.index_buffer_bound=
        binding.render_targets_ready=binding.barriers_ready=true;
    TlgNativeVk::Draw vk{};
    assert(TlgNativeVk::MakeJsDraw(source,binding,vk));
    unsigned calls[]={0,0};
    std::uint32_t args[6]{};
    TlgNativeVk::EmitGraphicsDraw(FakeCommandBuffer{calls,args},0,42,0,vk);
    assert(calls[0]==1 && calls[1]==1);
    assert(args[0]==42 && args[1]==220 && args[2]==1 &&
           args[3]==35 && args[4]==77 && args[5]==0);
    // The game context must never bind another context's Vulkan resources.
    binding.guest_context=0x2000;
    assert(!TlgNativeVk::MakeJsDraw(source,binding,vk));
    binding.guest_context=0x1000;
    // No submission if any GPU resource or synchronization state is unresolved.
    binding.barriers_ready=false;
    assert(!TlgNativeVk::MakeJsDraw(source,binding,vk));
    binding.barriers_ready=true;
    binding.index_capacity=200; // 35+220 exceeds index buffer.
    assert(!TlgNativeVk::MakeJsDraw(source,binding,vk));
    binding.index_capacity=1024;
    binding.index_binding_base=30;
    assert(TlgNativeVk::MakeJsDraw(source,binding,vk));
    assert(vk.first_index==5);
    binding.index_binding_base=40;
    assert(!TlgNativeVk::MakeJsDraw(source,binding,vk));
    binding.index_binding_base=0;
    binding.vertex_base=5;
    assert(!TlgNativeVk::MakeJsDraw(source,binding,vk));
    binding.vertex_base=77;
    record.ready=2; record.kind=TLG_JS_NATIVE_INSTANCED;
    record.arg0=25; record.arg1=4; record.arg2=10; record.arg3=77;
    assert(tlg_js_native_decode_draw(&record,&source));
    assert(TlgNativeVk::MakeJsDraw(source,binding,vk));
    assert(vk.vertex_or_index_count==25 && vk.instance_count==4 && vk.first_index==10);
    return 0;
}
