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
    std::puts("GPU native Js semantic collector PASS");
    return 0;
}
