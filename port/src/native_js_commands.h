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
    TLG_JS_NATIVE_INSTANCED = 2,
    TLG_JS_NATIVE_CREATE_BUFFER = 3,
    TLG_JS_NATIVE_CREATE_PIXEL_SHADER = 4,
    TLG_JS_NATIVE_DRAW = 5,
    TLG_JS_NATIVE_PREPARE_DRAW = 6
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

/* Capture the three shader-related native context slots read at the start
 * of v01.00 PrepareDraw, plus the original RSI/RDX arguments. Values remain
 * opaque until JsBuffer/JsShader object layouts have been verified.
 */
typedef struct TlgJsNativePrepareDrawCandidate {
    uint64_t sequence, context;
    uint64_t shader_slot_4678, shader_slot_4680, shader_slot_4688;
    uint64_t arg_rsi, arg_rdx;
} TlgJsNativePrepareDrawCandidate;

static inline int tlg_js_native_decode_prepare_draw(
    const TlgJsNativeCommand* event, TlgJsNativePrepareDrawCandidate* out) {
    if (!event || !out || !event->context ||
        event->kind != TLG_JS_NATIVE_PREPARE_DRAW)
        return 0;
    out->sequence = event->ready;
    out->context = event->context;
    out->shader_slot_4678 = event->arg0;
    out->shader_slot_4680 = event->arg1;
    out->shader_slot_4688 = event->arg2;
    out->arg_rsi = event->arg3;
    out->arg_rdx = event->arg4;
    return 1;
}

/* A semantic buffer-creation request is captured before the game has
 * assigned an output object. The last 16 bytes of the event are an immediate
 * JsBufferDesc snapshot from RSI, not the unused R8/R9 argument registers.
 * Do NOT dereference raw guest pointers in the GPU consumer; they may be
 * stack-local and expire on function return.
 * The actual native VkBuffer lifetime can only be attached after the resource
 * constructor's output semantics are fully validated.
 */
typedef struct TlgJsNativeBufferCreateCandidate {
    uint64_t sequence;
    uint64_t device;
    uint64_t description;
    uint64_t initial_data;
    uint64_t output_object;
    uint64_t description_head; /* first 8 bytes at entry */
    uint64_t description_tail; /* next 8 bytes at entry */
    uint32_t byte_width;       /* JsBufferDesc+0x00 */
    uint32_t usage;            /* JsBufferDesc+0x04, still guest-defined */
    uint32_t bind_flags;       /* JsBufferDesc+0x08, still guest-defined */
    uint32_t cpu_access_flags; /* JsBufferDesc+0x0c */
} TlgJsNativeBufferCreateCandidate;

static inline int tlg_js_native_decode_buffer_create(
    const TlgJsNativeCommand* record,
    TlgJsNativeBufferCreateCandidate* out) {
    if (!record || !out ||
        record->kind != TLG_JS_NATIVE_CREATE_BUFFER || !record->context)
        return 0;
    out->sequence = record->ready;
    out->device = record->context;
    out->description = record->arg0;
    out->initial_data = record->arg1;
    out->output_object = record->arg2;
    out->description_head = record->arg3;
    out->description_tail = record->arg4;
    out->byte_width = (uint32_t)record->arg3;
    out->usage = (uint32_t)(record->arg3 >> 32);
    out->bind_flags = (uint32_t)record->arg4;
    out->cpu_access_flags = (uint32_t)(record->arg4 >> 32);
    return 1;
}

/* The CreatePixelShader source pointer is captured at function entry;
 * recording its first 16 bytes avoids dereferencing a potentially expired
 * source blob on the GPU thread. ShaderBinaryInfo, its stage resource table,
 * final output object lifetime and any SPIR-V specialization are NOT known
 * from this event. It is telemetry, not permission to submit a native draw.
 */
typedef struct TlgJsNativeShaderCreateCandidate {
    uint64_t sequence;
    uint64_t device;
    uint64_t shader_blob;    /* original RSI; opaque after return */
    uint64_t input_rdx;      /* stage-specific argument; not yet interpreted */
    uint64_t output_rcx;     /* pointer to game-owned result; not dereferenced */
    uint64_t blob_head;      /* copy of bytes +0..+7 at entry */
    uint64_t blob_tail;      /* copy of bytes +8..+15 at entry */
} TlgJsNativeShaderCreateCandidate;

static inline int tlg_js_native_decode_pixel_shader_create(
    const TlgJsNativeCommand *entry, TlgJsNativeShaderCreateCandidate *out) {
    if (!entry || !out || !entry->context ||
        entry->kind != TLG_JS_NATIVE_CREATE_PIXEL_SHADER)
        return 0;
    out->sequence = entry->ready;
    out->device = entry->context;
    out->shader_blob = entry->arg0;
    out->input_rdx = entry->arg1;
    out->output_rcx = entry->arg2;
    out->blob_head = entry->arg3;
    out->blob_tail = entry->arg4;
    return 1;
}

/* JsDeviceContext::Draw (v01.00 guest+0x1312480), independent of
 * DrawIndexedCommon/DrawIndexedInstanced:
 * RDI=context, ESI=vertex count, EDX=firstVertex. GPU index bindings
 * MUST NOT be required when converting this semantic operation to Vulkan.
 */
typedef struct TlgJsNativeNonIndexedCandidate {
    uint64_t sequence, context;
    uint32_t vertex_count, first_vertex, instance_count;
} TlgJsNativeNonIndexedCandidate;

static inline int tlg_js_native_decode_nonindexed(
    const TlgJsNativeCommand *entry,
    TlgJsNativeNonIndexedCandidate *out) {
    if (!entry || !out || !entry->context ||
        entry->kind != TLG_JS_NATIVE_DRAW)
        return 0;
    out->sequence = entry->ready;
    out->context = entry->context;
    out->vertex_count = (uint32_t)entry->arg0; /* RSI */
    out->first_vertex = (uint32_t)entry->arg1; /* RDX */
    out->instance_count = 1;
    return 1;
}

/* High-level semantics reverse-engineered from the exact original v01.00 ELF:
 * guest+0x13198b0 JsDeviceContext::DrawIndexedCommon:
 *   RSI = index_count; RDX = index_offset; RCX = vertex_base;
 *   0x479c(context) <- RCX, and instance_count is forced to 1.
 * guest+0x1319a00 JsDeviceContext::DrawIndexedInstanced:
 *   RSI = index_count; RDX = instance_count; RCX = index_offset;
 *   R8 = vertex_base; 0x479c(context) <- R8.
 * Both functions then call sceGnmDrawIndexOffset with RDX=index_offset
 * and RCX=index_count. Do not confuse index_offset (guest buffer) with
 * Vulkan firstIndex when the index-buffer binding is already offset.
 */
typedef struct TlgJsNativeDrawCandidate {
    uint64_t sequence, context;
    uint32_t kind;
    uint32_t index_count, instance_count, first_index;
    uint32_t vertex_base;
    uint64_t flags_or_aux; /* preserve R8/R9, no guessed meaning */
} TlgJsNativeDrawCandidate;

static inline int tlg_js_native_decode_draw(const TlgJsNativeCommand *record,
                                             TlgJsNativeDrawCandidate *out) {
    if (!record || !out) return 0;
    if (record->kind != TLG_JS_NATIVE_INDEXED &&
        record->kind != TLG_JS_NATIVE_INSTANCED) return 0;
    const int instanced = record->kind == TLG_JS_NATIVE_INSTANCED;
    out->sequence = record->ready;
    out->context = record->context;
    out->kind = (uint32_t)record->kind;
    out->index_count = (uint32_t)record->arg0; /* RSI */
    out->instance_count = instanced ? (uint32_t)record->arg1 : 1; /* RDX */
    out->first_index = (uint32_t)(instanced ? record->arg2 : record->arg1);
    out->vertex_base = (uint32_t)(instanced ? record->arg3 : record->arg2);
    out->flags_or_aux = instanced ? record->arg4 : record->arg3;
    return 1;
}

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif
