/* SPDX-License-Identifier: GPL-2.0-or-later
 * Test the native Js->Vulkan shadow command ring without game files.
 */
#include "../src/native_js_commands.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void publish(TlgJsNativeRing *ring, uint64_t kind, uint64_t index) {
    const uint64_t at = __atomic_fetch_add(&ring->head, 1, __ATOMIC_ACQ_REL);
    TlgJsNativeCommand *r = &ring->records[at & (TLG_JS_COMMAND_CAPACITY - 1)];
    __atomic_store_n(&r->sequence, 0, __ATOMIC_RELEASE);
    r->kind = kind;
    r->context = 0xabcdef;
    r->rsi = index;
    r->rdx = 0x12;
    r->rcx = 0x34;
    r->r8 = 0x56;
    r->r9 = 0x78;
    __atomic_store_n(&r->sequence, at + 1, __ATOMIC_RELEASE);
}
int main(void) {
    TlgJsNativeRing *ring = calloc(1, sizeof(*ring));
    assert(ring);
    uint64_t cursor = 0, dropped = 0;
    TlgJsNativeCommand item;
    for (unsigned i = 0; i < 1000; ++i) publish(ring, TLG_JS_CMD_INDEXED_INSTANCED, i);
    for (unsigned i = 0; i < 1000; ++i) {
        assert(tlg_js_native_pop(ring, &cursor, &item, &dropped));
        assert(item.sequence == i + 1);
        assert(item.kind == TLG_JS_CMD_INDEXED_INSTANCED);
        assert(item.context == 0xabcdef && item.rsi == i);
        assert(item.rdx == 0x12 && item.rcx == 0x34);
        assert(item.r8 == 0x56 && item.r9 == 0x78);
    }
    assert(!tlg_js_native_pop(ring, &cursor, &item, &dropped));
    assert(!dropped);
    for (unsigned i = 0; i < TLG_JS_COMMAND_CAPACITY + 200; ++i)
        publish(ring, TLG_JS_CMD_INDEXED_COMMON, i);
    unsigned consumed = 0;
    while (tlg_js_native_pop(ring, &cursor, &item, &dropped)) {
        assert(item.kind == TLG_JS_CMD_INDEXED_COMMON);
        ++consumed;
    }
    assert(consumed == TLG_JS_COMMAND_CAPACITY && dropped == 200);
    free(ring);
    puts("Native Js->Vulkan shadow ring: ordering, payload, overflow PASS");
    return 0;
}
