/* SPDX-License-Identifier: GPL-2.0-or-later
 * Guest JsDeviceContext interception, phase 1 (read-only).
 *
 * Never call this a native renderer: this is the verified entry-point bridge
 * that lets a later Vulkan backend consume game-level draw parameters.
 * One v01.00 candidate is grounded in the supplied original executable.
 * Do not hook a different version from these addresses.
 */
#ifndef TLG_JS_NATIVE_BRIDGE_H
#define TLG_JS_NATIVE_BRIDGE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* .text virtual address relative to the eboot image, NOT a file offset.
 * Evidence: v01.00 DrawIndexedCommon assertion at guest+0x131990d,
 * inside the function whose entry is guest+0x13198b0.
 * x86-64 disassembly at 0x13198b0:
 * push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; mov rbx,rdi.
 * These fourteen bytes have no PC-relative instructions and are safe to replay.
 */
#define TLG_JS_100_INDEXED_ENTRY UINT64_C(0x13198b0)
#define TLG_JS_PROLOGUE_BYTES 14
#define TLG_JS_FINGERPRINT_BYTES 31
#define TLG_JS_TRAMPOLINE_BYTES (TLG_JS_PROLOGUE_BYTES + 14)
static const uint8_t tlg_js_100_indexed_original[TLG_JS_PROLOGUE_BYTES] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x54, 0x53, 0x48, 0x89, 0xfb,
};

/* Verify the actual DrawIndexedCommon body, not only a generic push/pop
 * prologue. Includes mov r15d,edx; mov r14d,esi; cmp [rbx+0x4724],1
 * and the following branch, observed in the provided v01.00 executable.
 */
static const uint8_t tlg_js_100_indexed_fingerprint[TLG_JS_FINGERPRINT_BYTES] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x54, 0x53, 0x48, 0x89, 0xfb,
    0x41, 0x89, 0xd7, 0x41, 0x89, 0xf6,
    0x83, 0xbb, 0x24, 0x47, 0x00, 0x00, 0x01,
    0x74, 0x0a, 0xc7, 0x83,
};

/* End-to-end source version and byte guards. We never scan blindly for code. */
static inline int tlg_js_100_signature_match(const uint8_t *image, size_t image_size) {
    const uint64_t at = TLG_JS_100_INDEXED_ENTRY;
    return image && image_size >= at + TLG_JS_FINGERPRINT_BYTES &&
           memcmp(image + at, tlg_js_100_indexed_fingerprint, TLG_JS_FINGERPRINT_BYTES) == 0;
}

/* x86-64: jmp qword ptr [rip+0], followed by the absolute destination.
 * Rel32 jumps can be out of range on Windows with ASLR.
 * The caller validates the target before making executable memory writable.
 */
static inline void tlg_js_write_absolute_jump(uint8_t *dst, uintptr_t destination) {
    const uint8_t op[6] = {0xff, 0x25, 0, 0, 0, 0};
    const uint64_t ptr = (uint64_t)destination;
    memcpy(dst, op, sizeof(op));
    memcpy(dst + 6, &ptr, sizeof(ptr));
}

static inline int tlg_js_build_trampoline(uint8_t *out, size_t capacity,
                                          const uint8_t *original, size_t length,
                                          uintptr_t continuation) {
    if (!out || !original || length != TLG_JS_PROLOGUE_BYTES ||
        capacity < TLG_JS_TRAMPOLINE_BYTES ||
        memcmp(original, tlg_js_100_indexed_original, length) != 0)
        return 0;
    memcpy(out, original, length);
    tlg_js_write_absolute_jump(out + length, continuation);
    return TLG_JS_TRAMPOLINE_BYTES;
}
#endif
