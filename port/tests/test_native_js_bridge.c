/* SPDX-License-Identifier: GPL-2.0-or-later
 * Isolated test: verified guest signature, absolute trampoline, SysV guest ABI.
 * Does not require proprietary game files, Vulkan, or shadPS4.
 */
#define _GNU_SOURCE
#include "../src/native_js_bridge.h"
#include "../src/native_js_commands.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

__attribute__((used, aligned(64))) TlgJsNativeRing tlg_js_native_ring;
__attribute__((used)) uintptr_t tlg_js_indexed_gateway;
__attribute__((used)) uintptr_t tlg_js_instanced_gateway;
__attribute__((used)) volatile uint64_t tlg_js_trace_instanced_calls;
__attribute__((used)) volatile uint64_t tlg_js_trace_indexed_calls;
__attribute__((used)) volatile uintptr_t tlg_js_trace_last_context;
volatile uintptr_t tlg_js_trace_last_arg1, tlg_js_trace_last_arg2, tlg_js_trace_last_arg3;

__attribute__((naked, used, noinline))
static void hook_entry(void) {
    __asm__ volatile(
        "pushfq\n\t"
        "lock incq tlg_js_trace_indexed_calls(%rip)\n\t"
        "movq %rdi, tlg_js_trace_last_context(%rip)\n\t"
        "movq %rsi, tlg_js_trace_last_arg1(%rip)\n\t"
        "movq %rdx, tlg_js_trace_last_arg2(%rip)\n\t"
        "movq %rcx, tlg_js_trace_last_arg3(%rip)\n\t"
        "pushq %rax\n\t"
        "pushq %r10\n\t"
        "pushq %r11\n\t"
        "leaq tlg_js_native_ring(%rip), %r10\n\t"
        "movq $1, %rax\n\t"
        "lock xaddq %rax, (%r10)\n\t"
        "leaq 1(%rax), %r11\n\t"
        "andq $4095, %rax\n\t"
        "shlq $6, %rax\n\t"
        "addq $64, %r10\n\t"
        "addq %rax, %r10\n\t"
        "movq $0, (%r10)\n\t"
        "movq 1, 8(%r10)\n\t"
        "movq %rdi, 16(%r10)\n\t"
        "movq %rsi, 24(%r10)\n\t"
        "movq %rdx, 32(%r10)\n\t"
        "movq %rcx, 40(%r10)\n\t"
        "movq %r8, 48(%r10)\n\t"
        "movq %r9, 56(%r10)\n\t"
        "movq %r11, (%r10)\n\t"
        "popq %r11\n\t"
        "popq %r10\n\t"
        "popq %rax\n\t"
        "popfq\n\t"
        "jmp *tlg_js_indexed_gateway(%rip)\n\t");
}
__attribute__((naked, used, noinline))
static void hook_instanced_entry(void) {
    __asm__ volatile(
        "pushfq\n\t"
        "lock incq tlg_js_trace_instanced_calls(%rip)\n\t"
        "pushq %rax\n\t"
        "pushq %r10\n\t"
        "pushq %r11\n\t"
        "leaq tlg_js_native_ring(%rip), %r10\n\t"
        "movq $1, %rax\n\t"
        "lock xaddq %rax, (%r10)\n\t"
        "leaq 1(%rax), %r11\n\t"
        "andq $4095, %rax\n\t"
        "shlq $6, %rax\n\t"
        "addq $64, %r10\n\t"
        "addq %rax, %r10\n\t"
        "movq $0, (%r10)\n\t"
        "movq 2, 8(%r10)\n\t"
        "movq %rdi, 16(%r10)\n\t"
        "movq %rsi, 24(%r10)\n\t"
        "movq %rdx, 32(%r10)\n\t"
        "movq %rcx, 40(%r10)\n\t"
        "movq %r8, 48(%r10)\n\t"
        "movq %r9, 56(%r10)\n\t"
        "movq %r11, (%r10)\n\t"
        "popq %r11\n\t"
        "popq %r10\n\t"
        "popq %rax\n\t"
        "popfq\n\t"
        "jmp *tlg_js_instanced_gateway(%rip)\n\t");
}
static unsigned char *exec_page(void) {
#ifdef _WIN32
    return (unsigned char*)VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE);
#else
    void *result = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    return result == MAP_FAILED ? NULL : result;
#endif
}

int main(void) {
    /* A 20 MB synthetic eboot image tests the exact offset guard; no game data. */
    const size_t n = TLG_JS_100_INSTANCED_ENTRY + 128;
    uint8_t *fake = (uint8_t *)calloc(n, 1);
    assert(fake);
    assert(!tlg_js_100_signature_match(fake, n));
    memcpy(fake + TLG_JS_100_INDEXED_ENTRY, tlg_js_100_indexed_fingerprint,
           TLG_JS_FINGERPRINT_BYTES);
    assert(tlg_js_100_signature_match(fake, n));
    memcpy(fake + TLG_JS_100_INSTANCED_ENTRY, tlg_js_100_instanced_fingerprint,
           TLG_JS_FINGERPRINT_BYTES);
    assert(tlg_js_100_instanced_signature_match(fake, n));
    assert(!tlg_js_100_instanced_signature_match(fake, TLG_JS_100_INSTANCED_ENTRY));
    fake[TLG_JS_100_INDEXED_ENTRY + 5] ^= 0x01;
    assert(!tlg_js_100_signature_match(fake, n));
    free(fake);

    unsigned char *machine = exec_page();
    assert(machine);
    /* Synthetic SysV function: the same 14-byte prologue, then pop the saved
     * registers, return 42. The hook must change no observable guest state.
     */
    static const unsigned char function[] = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
        0x41, 0x54, 0x53, 0x48, 0x89, 0xfb,
        0x5b, 0x41, 0x5c, 0x41, 0x5e, 0x41, 0x5f,
        0x5d, 0xb8, 0x2a, 0, 0, 0, 0xc3
    };
    memcpy(machine, function, sizeof(function));
    typedef int (__attribute__((sysv_abi)) *GuestFunction)(void *, void *, void *, void *);
    GuestFunction fn = (GuestFunction)machine;
    assert(fn((void *)(uintptr_t)0x123456, (void *)(uintptr_t)0x111, 
              (void *)(uintptr_t)0x222, (void *)(uintptr_t)0x333) == 42);
    unsigned char *trampoline = machine + 128;
    assert(tlg_js_build_trampoline(trampoline, 4096-128, machine,
                                    TLG_JS_PROLOGUE_BYTES,
                                    (uintptr_t)(machine + TLG_JS_PROLOGUE_BYTES)) ==
           TLG_JS_TRAMPOLINE_BYTES);
    tlg_js_indexed_gateway = (uintptr_t)trampoline;
    tlg_js_write_absolute_jump(machine, (uintptr_t)&hook_entry);
    __builtin___clear_cache((char *)machine, (char *)(machine + 64));
    assert(fn((void *)(uintptr_t)0x123456, (void *)(uintptr_t)0x111, 
              (void *)(uintptr_t)0x222, (void *)(uintptr_t)0x333) == 42);
    assert(fn((void *)(uintptr_t)0x654321, (void *)(uintptr_t)0x444,
              (void *)(uintptr_t)0x555, (void *)(uintptr_t)0x666) == 42);
    assert(tlg_js_trace_indexed_calls == 2);
    assert(tlg_js_trace_last_context == 0x654321);
    assert(tlg_js_trace_last_arg1 == 0x444);
    assert(tlg_js_trace_last_arg2 == 0x555);
    assert(tlg_js_trace_last_arg3 == 0x666);
    /* Second verified v01.00 high-level entry: different prologue and
     * signature, same safe trampoline structure. */
    static const unsigned char function_instanced[] = {
        0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
        0x41, 0x55, 0x41, 0x54, 0x53, 0x50,
        0x58, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e,
        0x41, 0x5f, 0x5d, 0xb8, 0x2a, 0, 0, 0, 0xc3,
    };
    memcpy(machine + 512, function_instanced, sizeof(function_instanced));
    GuestFunction instanced_fn = (GuestFunction)(machine + 512);
    assert(instanced_fn((void *)(uintptr_t)0x234567, 0, 0, 0) == 42);
    assert(tlg_js_build_trampoline(machine + 640, 4096 - 640, machine + 512,
                                   TLG_JS_PROLOGUE_BYTES,
                                   (uintptr_t)(machine + 512 + TLG_JS_PROLOGUE_BYTES)) ==
           TLG_JS_TRAMPOLINE_BYTES);
    tlg_js_instanced_gateway = (uintptr_t)(machine + 640);
    tlg_js_write_absolute_jump(machine + 512, (uintptr_t)&hook_instanced_entry);
    __builtin___clear_cache((char *)(machine + 512), (char *)(machine + 576));
    assert(instanced_fn((void *)(uintptr_t)0x234567, 0, 0, 0) == 42);
    assert(instanced_fn((void *)(uintptr_t)0x345678, 0, 0, 0) == 42);
    assert(tlg_js_trace_instanced_calls == 2);

    /* End-to-end: hooks, raw registers, immutable sequence, and C/C++ reader
     * agree on layout. The GPU side uses this same pop function in OnSubmit. */
    uint64_t cursor = 0, lost = 0;
    TlgJsNativeCommand entry;
    const uintptr_t contexts[] = {0x123456, 0x654321, 0x234567, 0x345678};
    const uint64_t kinds[] = {TLG_JS_CMD_INDEXED_COMMON, TLG_JS_CMD_INDEXED_COMMON,
                              TLG_JS_CMD_INDEXED_INSTANCED, TLG_JS_CMD_INDEXED_INSTANCED};
    for (unsigned n = 0; n < 4; ++n) {
        assert(tlg_js_native_pop(&tlg_js_native_ring, &cursor, &entry, &lost));
        assert(entry.sequence == n + 1 && entry.kind == kinds[n]);
        assert(entry.context == contexts[n]);
    }
    assert(!lost && !tlg_js_native_pop(&tlg_js_native_ring, &cursor, &entry, &lost));
#ifdef _WIN32
    VirtualFree(machine, 0, MEM_RELEASE);
#else
    munmap(machine, 4096);
#endif
    puts("Native Js guest-entry hook: ABI, signature and trampoline PASS");
    return 0;
}
