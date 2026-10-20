// SPDX-License-Identifier: GPL-2.0-or-later
// Native high-level Draw -> PrepareDraw pairing, without a Vulkan driver.
#include "../gpu/shim/tlg_js_prepare_match.h"
#include <cassert>

static TlgJsNativeCommand Event(std::uint64_t seq, std::uint64_t ctx,
                                std::uint64_t kind) {
    TlgJsNativeCommand event{};
    event.ready = seq;
    event.context = ctx;
    event.kind = kind;
    return event;
}
int main() {
    using namespace TlgNativeJs;
    PrepareDrawMatcher matcher;
    PreparedDrawSnapshot pair{};

    auto drawA = Event(1, 0x111, TLG_JS_NATIVE_INDEXED);
    drawA.arg0 = 24;
    drawA.arg1 = 4;
    drawA.arg2 = 7;
    auto drawB = Event(2, 0x222, TLG_JS_NATIVE_DRAW);
    drawB.arg0 = 6;
    drawB.arg1 = 2;
    assert(!matcher.Observe(drawA, pair));
    assert(!matcher.Observe(drawB, pair));
    assert(matcher.PendingCount() == 2);
    auto stateA = Event(3, 0x111, TLG_JS_NATIVE_PREPARE_DRAW);
    stateA.arg0 = 0x10001;
    stateA.arg1 = 0x20002;
    stateA.arg2 = 0x30003;
    assert(matcher.Observe(stateA, pair));
    assert(pair.context == 0x111 && pair.is_indexed);
    assert(pair.indexed.index_count == 24 && pair.indexed.first_index == 4);
    assert(pair.slot_4678 == 0x10001 && pair.slot_4688 == 0x30003);
    assert(pair.draw_sequence == 1 && pair.preparation_sequence == 3);
    assert(matcher.PendingCount() == 1);

    auto unrelated = Event(4, 0x999, TLG_JS_NATIVE_PREPARE_DRAW);
    assert(!matcher.Observe(unrelated, pair));
    auto stateB = Event(5, 0x222, TLG_JS_NATIVE_PREPARE_DRAW);
    stateB.arg0 = 0x5678;
    assert(matcher.Observe(stateB, pair));
    assert(!pair.is_indexed && pair.ordinary.vertex_count == 6);
    assert(pair.ordinary.first_vertex == 2);
    assert(pair.slot_4678 == 0x5678);
    assert(matcher.PairedCount() == 2);
    assert(matcher.PendingCount() == 0);

    auto stale = Event(7, 0x333, TLG_JS_NATIVE_DRAW);
    stale.arg0 = 6;
    assert(!matcher.Observe(stale, pair));
    matcher.ResetAfterLoss(); // command-ring overflow or missed guest calls
    assert(matcher.PendingCount() == 0 && matcher.ResetCount() == 1);
    auto new_state = Event(8, 0x333, TLG_JS_NATIVE_PREPARE_DRAW);
    assert(!matcher.Observe(new_state, pair));
    // A reordered or repeated event is never paired.
    auto next = Event(10, 0x333, TLG_JS_NATIVE_INSTANCED);
    next.arg0 = 3; next.arg1 = 2; next.arg2 = 1;
    assert(!matcher.Observe(next, pair));
    new_state.ready = 9;
    assert(!matcher.Observe(new_state, pair));
    assert(matcher.PairedCount() == 2);
    return 0;
}
