/* SPDX-License-Identifier: GPL-2.0-or-later
 * Synthetic v01.00 PrepareDraw high-level context state capture.
 * Verifies original SysV ABI is preserved through a 24-byte trampoline.
 */
#define _GNU_SOURCE
#include "../src/native_js_bridge.h"
#include "../src/native_js_commands.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

__attribute__((used,aligned(64))) TlgJsNativeRing tlg_js_native_ring;
__attribute__((used)) uintptr_t tlg_js_prepare_test_gateway;

__attribute__((naked, used, noinline))
static void hook(void) {
    __asm__ volatile(
        "pushfq\n\t"
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
        "movq $6, 8(%r10)\n\t"
        "movq %rdi, 16(%r10)\n\t"
        "movq 0x4678(%rdi), %rax\n\t"
        "movq %rax, 24(%r10)\n\t"
        "movq 0x4680(%rdi), %rax\n\t"
        "movq %rax, 32(%r10)\n\t"
        "movq 0x4688(%rdi), %rax\n\t"
        "movq %rax, 40(%r10)\n\t"
        "movq %rsi, 48(%r10)\n\t"
        "movq %rdx, 56(%r10)\n\t"
        "movq %r11, (%r10)\n\t"
        "popq %r11\n\t"
        "popq %r10\n\t"
        "popq %rax\n\t"
        "popfq\n\t"
        "jmp *tlg_js_prepare_test_gateway(%rip)\n\t"
    );
}

typedef int (__attribute__((sysv_abi))* GuestFn)(void*,unsigned,unsigned);
static unsigned char* exec_page(void) {
#ifdef _WIN32
    return (unsigned char*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
#else
    void* p=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return p==MAP_FAILED?NULL:p;
#endif
}
int main(void) {
    const size_t size=TLG_JS_100_PREPARE_DRAW_ENTRY+TLG_JS_PREPARE_DRAW_FINGERPRINT_BYTES+1;
    uint8_t* fake=calloc(size,1);
    assert(fake);
    assert(!tlg_js_100_prepare_draw_signature_match(fake,size));
    memcpy(fake+TLG_JS_100_PREPARE_DRAW_ENTRY,
           tlg_js_100_prepare_draw_fingerprint,TLG_JS_PREPARE_DRAW_FINGERPRINT_BYTES);
    assert(tlg_js_100_prepare_draw_signature_match(fake,size));
    fake[TLG_JS_100_PREPARE_DRAW_ENTRY+27]^=1;
    assert(!tlg_js_100_prepare_draw_signature_match(fake,size));
    free(fake);
    unsigned char* m=exec_page();
    assert(m);
    static const uint8_t body[]={
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,
        0x53,0x48,0x83,0xe4,0xe0,0x48,0x81,0xec,0x80,0x05,0x00,0x00,
        0xb8,42,0,0,0, /* mov eax,42 */
        0x48,0x89,0xec, /* mov rsp,rbp; skip unchanged saved regs */
        0x5d,0xc3       /* pop rbp, ret */
    };
    memcpy(m,body,sizeof(body));
    GuestFn guest=(GuestFn)m;
    unsigned char* context=calloc(0x4700,1);
    assert(context);
    uint64_t s0=0xaabbcc01,s1=0xaabbcc02,s2=0xaabbcc03;
    memcpy(context+0x4678,&s0,8);
    memcpy(context+0x4680,&s1,8);
    memcpy(context+0x4688,&s2,8);
    assert(guest(context,10,20)==42);
    uint8_t* tramp=m+128;
    assert(tlg_js_build_prepare_draw_trampoline(
        tramp,4096-128,m,TLG_JS_PREPARE_DRAW_PROLOGUE_BYTES,
        (uintptr_t)(m+TLG_JS_PREPARE_DRAW_PROLOGUE_BYTES))==TLG_JS_PREPARE_DRAW_TRAMPOLINE_BYTES);
    tlg_js_prepare_test_gateway=(uintptr_t)tramp;
    tlg_js_write_absolute_jump(m,(uintptr_t)hook);
    memset(m+14,0x90,TLG_JS_PREPARE_DRAW_PROLOGUE_BYTES-14);
    __builtin___clear_cache((char*)m,(char*)m+64);
    assert(guest(context,10,20)==42);
    uint64_t cursor=0,dropped=0;
    TlgJsNativeCommand raw{};
    TlgJsNativePrepareDrawCandidate state{};
    assert(tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&raw,&dropped));
    assert(tlg_js_native_decode_prepare_draw(&raw,&state));
    assert(state.context==(uintptr_t)context);
    assert(state.shader_slot_4678==s0 && state.shader_slot_4680==s1 &&
           state.shader_slot_4688==s2);
    assert(state.arg_rsi==10 && state.arg_rdx==20);
    assert(!dropped);
#ifdef _WIN32
    VirtualFree(m,0,MEM_RELEASE);
#else
    munmap(m,4096);
#endif
    free(context);
    puts("v01.00 PrepareDraw state capture, 24-byte trampoline and ABI PASS");
    return 0;
}
