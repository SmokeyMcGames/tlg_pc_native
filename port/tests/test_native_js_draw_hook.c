/* SPDX-License-Identifier: GPL-2.0-or-later
 * Native v01.00 JsDeviceContext::Draw interception: synthetic x86-64
 * game function, exact verified signature, ABI/trampoline and semantics.
 */
#define _GNU_SOURCE
#include "../src/native_js_bridge.h"
#include "../src/native_js_commands.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

__attribute__((used,aligned(64))) TlgJsNativeRing tlg_js_native_ring;
__attribute__((used)) uintptr_t tlg_js_draw_test_gateway;
__attribute__((used)) volatile uint64_t tlg_js_draw_test_calls;
__attribute__((naked, used, noinline))
static void hooked(void) {
    __asm__ volatile(
        "pushfq\n\t"
        "pushq %rax\n\t"
        "pushq %r10\n\t"
        "pushq %r11\n\t"
        "lock incq tlg_js_draw_test_calls(%rip)\n\t"
        "leaq tlg_js_native_ring(%rip), %r10\n\t"
        "movq $1, %rax\n\t"
        "lock xaddq %rax, (%r10)\n\t"
        "leaq 1(%rax), %r11\n\t"
        "andq $4095, %rax\n\t"
        "shlq $6, %rax\n\t"
        "addq $64, %r10\n\t"
        "addq %rax, %r10\n\t"
        "movq $0, (%r10)\n\t"
        "movq $5, 8(%r10)\n\t"
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
        "jmp *tlg_js_draw_test_gateway(%rip)\n\t");
}

typedef int (__attribute__((sysv_abi)) *GuestFn)(void*,uint32_t,uint32_t);
static unsigned char* executable_page(void) {
#ifdef _WIN32
    return (unsigned char*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,
                                        PAGE_EXECUTE_READWRITE);
#else
    void* p = mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                   MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
int main(void) {
    const size_t size = TLG_JS_100_DRAW_ENTRY + TLG_JS_DRAW_FINGERPRINT_BYTES + 1;
    uint8_t* fake = calloc(size, 1);
    assert(fake);
    assert(!tlg_js_100_draw_signature_match(fake,size));
    memcpy(fake+TLG_JS_100_DRAW_ENTRY,tlg_js_100_draw_fingerprint,
           TLG_JS_DRAW_FINGERPRINT_BYTES);
    assert(tlg_js_100_draw_signature_match(fake,size));
    fake[TLG_JS_100_DRAW_ENTRY+24] ^= 1;
    assert(!tlg_js_100_draw_signature_match(fake,size));
    free(fake);

    unsigned char* machine = executable_page();
    assert(machine);
    static const uint8_t function[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,
        0x81,0xec,0x20,0x02,0,0,
        0xb8,0x2a,0,0,0,
        0x48,0x81,0xc4,0x20,0x02,0,0,
        0x5b,0x41,0x5c,0x41,0x5e,0x41,0x5f,0x5d,0xc3,
    };
    memcpy(machine,function,sizeof(function));
    GuestFn fn=(GuestFn)machine;
    assert(fn((void*)(uintptr_t)0x1000,6,2)==42);
    unsigned char* tramp=machine+128;
    assert(tlg_js_build_draw_trampoline(tramp,4096-128,machine,
        TLG_JS_DRAW_PROLOGUE_BYTES,(uintptr_t)(machine+TLG_JS_DRAW_PROLOGUE_BYTES))
        == TLG_JS_DRAW_TRAMPOLINE_BYTES);
    tlg_js_draw_test_gateway=(uintptr_t)tramp;
    tlg_js_write_absolute_jump(machine,(uintptr_t)&hooked);
    memset(machine+14,0x90,TLG_JS_DRAW_PROLOGUE_BYTES-14);
    __builtin___clear_cache((char*)machine,(char*)machine+64);
    assert(fn((void*)(uintptr_t)0x1100,6,2)==42);
    assert(fn((void*)(uintptr_t)0x1200,18,7)==42);
    assert(tlg_js_draw_test_calls==2);
    uint64_t cursor=0,dropped=0;
    TlgJsNativeCommand command{};
    TlgJsNativeNonIndexedCandidate draw{};
    assert(tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&command,&dropped));
    assert(tlg_js_native_decode_nonindexed(&command,&draw));
    assert(draw.sequence==1 && draw.context==0x1100 &&
           draw.vertex_count==6 && draw.first_vertex==2);
    assert(tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&command,&dropped));
    assert(tlg_js_native_decode_nonindexed(&command,&draw));
    assert(draw.sequence==2 && draw.context==0x1200 &&
           draw.vertex_count==18 && draw.first_vertex==7);
    assert(!tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&command,&dropped));
    assert(!dropped);
#ifdef _WIN32
    VirtualFree(machine,0,MEM_RELEASE);
#else
    munmap(machine,4096);
#endif
    puts("v01.00 nonindexed native Js Draw hook and guest ABI PASS");
    return 0;
}
