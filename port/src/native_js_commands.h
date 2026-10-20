/* SPDX-License-Identifier: GPL-2.0-or-later
 * Native Vulkan renderer research: game-level JsDeviceContext draw command stream.
 *
 * This captures semantic draw arguments BEFORE the game's PS4 Gnm encoder
 * executes. An eventual native Vulkan backend can consume these records and
 * map their resources independently from the legacy PM4 decoder. Until the
 * native state tracker is complete, this is a strictly read-only shadow stream.
 *
 * All slots are 64 bytes; both code versions use the same C layout.
 * Native hooks are opt-in and gated by validated v01.00 function signatures.
 */
#ifndef TLG_NATIVE_JS_COMMANDS_H
#define TLG_NATIVE_JS_COMMANDS_H

#include <stdint.h>
#include <stddef.h>

#define TLG_JS_NATIVE_RING_CAPACITY 4096u
#define TLG_JS_NATIVE_RING_MASK (TLG_JS_NATIVE_RING_CAPACITY - 1u)
#define TLG_JS_NATIVE_RECORD_BYTES 64u

enum {
    TLG_JS_NATIVE_INDEXED = 1,
    TLG_JS_NATIVE_INSTANCED = 2
};

/* Full 64-bit register values; never dereference game pointers in a capture hook. */
typedef struct TlgJsNativeCommand {
    uint64_t ready;   /* 1-based producer sequence, written LAST */
    uint64_t kind;
    uint64_t context; /* game-owned JsDeviceContext */
    uint64_t arg0;    /* entry RSI */
    uint64_t arg1;    /* entry RDX */
    uint64_t arg2;    /* entry RCX */
    uint64_t arg3;    /* entry R8 */
    uint64_t arg4;    /* entry R9 */
} TlgJsNativeCommand;

typedef struct TlgJsNativeRing {
    uint64_t head; /* atomic XADD; number of reserved records */
    uint64_t padding[7];
    TlgJsNativeCommand commands[TLG_JS_NATIVE_RING_CAPACITY];
} TlgJsNativeRing;

#ifdef __cplusplus
static_assert(sizeof(TlgJsNativeCommand) == TLG_JS_NATIVE_RECORD_BYTES);
static_assert(offsetof(TlgJsNativeRing, commands) == 64);
extern "C" {
#else
_Static_assert(sizeof(TlgJsNativeCommand) == TLG_JS_NATIVE_RECORD_BYTES, "64-byte record");
_Static_assert(offsetof(TlgJsNativeRing, commands) == 64, "64-byte header");
#endif

extern TlgJsNativeRing tlg_js_native_ring;

/* Consume one immutable snapshot. Never block a guest or GPU thread.
 * A producer may overwrite old telemetry after 4096 entries; report the loss.
 * This reader uses atomic loads on EVERY field, plus a second generation check.
 * It NEVER dereferences 'context' or any argument (opaque guest values).
 */
static inline int tlg_js_native_read_next(const TlgJsNativeRing *ring,
                                           uint64_t *cursor,
                                           TlgJsNativeCommand *out,
                                           uint64_t *dropped) {
    if (!ring || !cursor || !out || !dropped) return 0;
    const uint64_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    if (head <= *cursor) return 0;
    if (head - *cursor > TLG_JS_NATIVE_RING_CAPACITY) {
        const uint64_t loss = head - *cursor - TLG_JS_NATIVE_RING_CAPACITY;
        *dropped += loss;
        *cursor = head - TLG_JS_NATIVE_RING_CAPACITY;
    }
    const TlgJsNativeCommand *record = &ring->commands[*cursor & TLG_JS_NATIVE_RING_MASK];
    const uint64_t wanted = *cursor + 1;
    if (__atomic_load_n(&record->ready, __ATOMIC_ACQUIRE) != wanted) return 0;
    TlgJsNativeCommand current;
    current.ready = wanted;
    current.kind = __atomic_load_n(&record->kind, __ATOMIC_RELAXED);
    current.context = __atomic_load_n(&record->context, __ATOMIC_RELAXED);
    current.arg0 = __atomic_load_n(&record->arg0, __ATOMIC_RELAXED);
    current.arg1 = __atomic_load_n(&record->arg1, __ATOMIC_RELAXED);
    current.arg2 = __atomic_load_n(&record->arg2, __ATOMIC_RELAXED);
    current.arg3 = __atomic_load_n(&record->arg3, __ATOMIC_RELAXED);
    current.arg4 = __atomic_load_n(&record->arg4, __ATOMIC_RELAXED);
    if (__atomic_load_n(&record->ready, __ATOMIC_ACQUIRE) != wanted) return 0;
    *out = current;
    ++*cursor;
    return 1;
}

/* Pure translation to a *semantic candidate*; no GPU work is submitted.
 * The 1.00 DrawIndexedCommon code passes original RSI(indexCount) and
 * RDX(indexOffset) to the Gnm encoder.
 * DrawIndexedInstanced passes RSI(indexCount), RDX(instanceCount),
 * RCX(indexOffset). Other parameters remain opaque until their layout
 * is verified from the original game's code and visual parity tests.
 */
typedef struct TlgJsNativeDrawCandidate {
    uint64_t sequence, context;
    uint32_t kind;
    uint32_t index_count, instance_count, first_index;
    uint64_t arg3_raw, arg4_raw;
} TlgJsNativeDrawCandidate;

static inline int tlg_js_native_decode_draw(const TlgJsNativeCommand *record,
                                              TlgJsNativeDrawCandidate *out) {
    if (!record || !out) return 0;
    if (record->kind != TLG_JS_NATIVE_INDEXED &&
        record->kind != TLG_JS_NATIVE_INSTANCED) return 0;
    out->sequence = record->ready;
    out->context = record->context;
    out->kind = (uint32_t)record->kind;
    out->index_count = (uint32_t)record->arg0;
    out->instance_count = record->kind == TLG_JS_NATIVE_INSTANCED
                              ? (uint32_t)record->arg1 : 1;
    out->first_index = (uint32_t)(record->kind == TLG_JS_NATIVE_INSTANCED
                                     ? record->arg2 : record->arg1);
    out->arg3_raw = record->arg3;
    out->arg4_raw = record->arg4;
    return 1;
}

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif
