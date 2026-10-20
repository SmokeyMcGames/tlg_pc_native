// SPDX-License-Identifier: GPL-2.0-or-later
// GPU-side semantic command collector test: no Vulkan driver or game files.
#include "../gpu/shim/tlg_js_shadow.h"
#include "../gpu/tlggpu.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

struct Fixture {
    unsigned next = 0;
    static constexpr unsigned N = 3;
    TlgJsNativeDrawCandidate commands[N] = {
        {.sequence=1, .context=0x1234, .kind=TLG_JS_NATIVE_INDEXED,
         .index_count=500, .instance_count=1, .first_index=50},
        {.sequence=2, .context=0x5678, .kind=TLG_JS_NATIVE_INSTANCED,
         .index_count=1000, .instance_count=4, .first_index=20},
        {.sequence=3, .context=0x9999, .kind=99}
    };
};
extern "C" int supply(void *userdata, TlgJsNativeDrawCandidate *out,
                       std::uint64_t *dropped) {
    auto* f = static_cast<Fixture*>(userdata);
    *dropped = f->next == 1 ? 3 : 0;
    if (f->next >= Fixture::N) return 0;
    *out = f->commands[f->next++];
    return 1;
}
struct EventFixture {
    unsigned next = 0;
    TlgJsNativeCommand commands[4] = {
        {.ready=1, .kind=TLG_JS_NATIVE_CREATE_BUFFER,
         .context=0x111, .arg0=0x222, .arg1=0x333, .arg2=0x444,
         .arg3=(std::uint64_t{2} << 32) | 4096,
         .arg4=(std::uint64_t{8} << 32) | 4},
        {.ready=2, .kind=TLG_JS_NATIVE_INDEXED,
         .context=0x555, .arg0=36, .arg1=8, .arg2=4},
        {.ready=3, .kind=TLG_JS_NATIVE_INSTANCED,
         .context=0x555, .arg0=18, .arg1=2, .arg2=3, .arg3=4},
        {.ready=4, .kind=TLG_JS_NATIVE_CREATE_PIXEL_SHADER,
         .context=0x900, .arg0=0x901, .arg1=0x902, .arg2=0x903,
         .arg3=0x1122334455667788ull, .arg4=0x99aabbccddeeff00ull}
    };
};
extern "C" int supply_events(void *userdata, TlgJsNativeCommand *out,
                              std::uint64_t *dropped) {
    auto* f = static_cast<EventFixture*>(userdata);
    *dropped = 0;
    if (f->next >= 4) return 0;
    *out = f->commands[f->next++];
    return 1;
}

int main() {
    Fixture fixture;
    TlgNativeJs::ResetForTest();
    tlggpu_set_native_js_draw_source(&supply,&fixture);
    TlgNativeJs::PumpOnGpuSubmission();
    const auto stats=TlgNativeJs::Snapshot();
    assert(stats.indexed == 1);
    assert(stats.indexed_instanced == 1);
    assert(stats.invalid == 1);
    assert(stats.dropped == 3);
    assert(stats.batches == 1);
    assert(stats.last_context == 0x5678);
    assert(stats.last_sequence == 2);
    tlggpu_set_native_js_draw_source(nullptr,nullptr);
    TlgNativeJs::PumpOnGpuSubmission();
    assert(TlgNativeJs::Snapshot().batches == 1);
    // Typed high-level stream takes precedence over the legacy draw-only API.
    EventFixture events;
    TlgNativeJs::ResetForTest();
    tlggpu_set_native_js_event_source(&supply_events, &events);
    TlgNativeJs::PumpOnGpuSubmission();
    const auto native = TlgNativeJs::Snapshot();
    assert(native.indexed == 1);
    assert(native.indexed_instanced == 1);
    assert(native.buffer_creates == 1);
    assert(native.pixel_shader_creates == 1);
    assert(native.last_shader_head == 0x1122334455667788ull);
    assert(native.last_shader_tail == 0x99aabbccddeeff00ull);
    assert(native.last_buffer_description == 0x222);
    assert(native.last_buffer_bytes == 4096);
    assert(native.last_buffer_usage == 2);
    assert(native.last_buffer_bind_flags == 4);
    assert(native.last_buffer_cpu_access == 8);
    assert(native.invalid == 0);
    assert(native.last_sequence == 4);
    TlgNativeJs::ResetForTest();
    std::puts("GPU native Js semantic collector PASS");
    return 0;
}
