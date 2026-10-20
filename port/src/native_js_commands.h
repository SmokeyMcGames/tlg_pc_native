/* SPDX-License-Identifier: GPL-2.0-or-later
 * Shadow command stream for guest JsDeviceContext entry calls.
 * This is NOT a Vulkan draw implementation. The backend consumes the events
 * for parity/coverage research while legacy PM4 still draws the actual frame.
 *
 * Two high-level game functions are hooked only on a fingerprint-matched
 * v1.00 binary. Each producer reserves a ticket and fills an aligned slot;
 * the commit sequence is written last. The GPU thread never stalls waiting
 * for a producer, and reports drops on overflow. Each record is 64 bytes.
 */
#ifndef TLG_NATIVE_JS_COMMANDS_H
#define TLG_NATIVE_JS_COMMANDS_H
#include <stdint.h>
#include <stddef.h>

enum { TLG_JS_COMMAND_CAPACITY = 4096 };
enum { TLG_JS_CMD_INDEXED_COMMON = 1, TLG_JS_CMD_INDEXED_INSTANCED = 2 };
typedef struct TlgJsNativeCommand {
    uint64_t sequence; /* ticket + 1 when fully committed, 0 during write */
    uint64_t kind;
    uintptr_t context; /* guest this pointer */
    uint64_t rsi, rdx, rcx, r8, r9; /* raw SysV guest arguments */
} TlgJsNativeCommand;

typedef struct TlgJsNativeRing {
    uint64_t head; /* globally reserved tickets; not the number committed */
    uint64_t padding[7]; /* separate producer head's cache line from records */
    TlgJsNativeCommand records[TLG_JS_COMMAND_CAPACITY];
} TlgJsNativeRing;

#ifdef __cplusplus
static_assert(sizeof(TlgJsNativeCommand) == 64);
static_assert(offsetof(TlgJsNativeRing, records) == 64);
#else
_Static_assert(sizeof(TlgJsNativeCommand) == 64, "64-byte command");
_Static_assert(offsetof(TlgJsNativeRing, records) == 64, "64-byte head padding");
#endif

/* Called on the GPU command thread. Returns 1 for one stable entry, 0 if
 * empty/pending/overwritten. Never reads guest pointers or blocks.
 * The ring is best-effort instrumentation, NOT a reliable rendering transport.
 */
static inline int tlg_js_native_pop(const TlgJsNativeRing *ring, uint64_t *cursor,
                                    TlgJsNativeCommand *out, uint64_t *dropped) {
    if (!ring || !cursor || !out) return 0;
    const uint64_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    if (head <= *cursor) return 0;
    if (head - *cursor > TLG_JS_COMMAND_CAPACITY) {
        const uint64_t lost = head - *cursor - TLG_JS_COMMAND_CAPACITY;
        if (dropped) *dropped += lost;
        *cursor = head - TLG_JS_COMMAND_CAPACITY;
    }
    const uint64_t wanted = *cursor + 1;
    const TlgJsNativeCommand *slot = &ring->records[*cursor & (TLG_JS_COMMAND_CAPACITY - 1)];
    if (__atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE) != wanted) return 0;
    out->sequence = wanted;
    out->kind = __atomic_load_n(&slot->kind, __ATOMIC_RELAXED);
    out->context = __atomic_load_n(&slot->context, __ATOMIC_RELAXED);
    out->rsi = __atomic_load_n(&slot->rsi, __ATOMIC_RELAXED);
    out->rdx = __atomic_load_n(&slot->rdx, __ATOMIC_RELAXED);
    out->rcx = __atomic_load_n(&slot->rcx, __ATOMIC_RELAXED);
    out->r8 = __atomic_load_n(&slot->r8, __ATOMIC_RELAXED);
    out->r9 = __atomic_load_n(&slot->r9, __ATOMIC_RELAXED);
    if (__atomic_load_n(&slot->sequence, __ATOMIC_ACQUIRE) != wanted) return 0;
    ++*cursor;
    return 1;
}
#endif
