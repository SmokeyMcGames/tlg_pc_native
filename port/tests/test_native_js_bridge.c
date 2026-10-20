/* SPDX-License-Identifier: GPL-2.0-or-later
 * Isolated test: verified guest signature, absolute trampoline, SysV guest ABI.
 * Does not require proprietary game files, Vulkan, or shadPS4.
 */
#define _GNU_SOURCE
#include "../src/native_js_bridge.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

__attribute__((used)) uintptr_t tlg_js_indexed_gateway;
__attribute__((used)) volatile uint64_t tlg_js_trace_indexed_calls;
__attribute__((used)) volatile uintptr_t tlg_js_trace_last_context;

__attribute__((naked, used, noinline))
static void hook_entry(void) {
    __asm__ volatile(
        "pushfq\n\t"
        "lock incq tlg_js_trace_indexed_calls(%rip)\n\t"
        "movq %rdi, tlg_js_trace_last_context(%rip)\n\t"
        "popfq\n\t"
        "jmp *tlg_js_indexed_gateway(%rip)\n\t");
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
    const size_t n = TLG_JS_100_INDEXED_ENTRY + 128;
    uint8_t *fake = (uint8_t *)calloc(n, 1);
    assert(fake);
    assert(!tlg_js_100_signature_match(fake, n));
    memcpy(fake + TLG_JS_100_INDEXED_ENTRY, tlg_js_100_indexed_original,
           TLG_JS_PROLOGUE_BYTES);
    assert(tlg_js_100_signature_match(fake, n));
    assert(!tlg_js_100_signature_match(fake, n - 129));
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
        0x41, 0x55, 0x41, 0x54, 0x53, 0x50,
        0x58, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e,
        0x41, 0x5f, 0x5d, 0xb8, 0x2a, 0, 0, 0, 0xc3
    };
    memcpy(machine, function, sizeof(function));
    typedef int (__attribute__((sysv_abi)) *GuestFunction)(void *);
    GuestFunction fn = (GuestFunction)machine;
    assert(fn((void *)(uintptr_t)0x123456) == 42);
    unsigned char *trampoline = machine + 128;
    assert(tlg_js_build_trampoline(trampoline, 4096-128, machine,
                                    TLG_JS_PROLOGUE_BYTES,
                                    (uintptr_t)(machine + TLG_JS_PROLOGUE_BYTES)) ==
           TLG_JS_TRAMPOLINE_BYTES);
    tlg_js_indexed_gateway = (uintptr_t)trampoline;
    tlg_js_write_absolute_jump(machine, (uintptr_t)&hook_entry);
    __builtin___clear_cache((char *)machine, (char *)(machine + 64));
    assert(fn((void *)(uintptr_t)0x123456) == 42);
    assert(fn((void *)(uintptr_t)0x654321) == 42);
    assert(tlg_js_trace_indexed_calls == 2);
    assert(tlg_js_trace_last_context == 0x654321);
#ifdef _WIN32
    VirtualFree(machine, 0, MEM_RELEASE);
#else
    munmap(machine, 4096);
#endif
    puts("Native Js guest-entry hook: ABI, signature and trampoline PASS");
    return 0;
}
