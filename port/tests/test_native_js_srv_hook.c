// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone v1.00 JsDevice::CreateShaderResourceView ABI/trampoline test.
// All guest bytes below are synthetic; no game assets or Vulkan are required.
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
__attribute__((used)) uintptr_t tlg_js_test_srv_gateway;
__attribute__((used)) volatile uint64_t tlg_js_test_srv_calls;

__attribute__((naked,used,noinline))
static void hook(void) {
    __asm__ volatile(
        "pushfq\n\t"
        "pushq %rax\n\t"
        "pushq %r10\n\t"
        "pushq %r11\n\t"
        "lock incq tlg_js_test_srv_calls(%rip)\n\t"
        "leaq tlg_js_native_ring(%rip), %r10\n\t"
        "movq $1, %rax\n\t"
        "lock xaddq %rax, (%r10)\n\t"
        "leaq 1(%rax), %r11\n\t"
        "andq $4095, %rax\n\t"
        "shlq $6, %rax\n\t"
        "addq $64, %r10\n\t"
        "addq %rax, %r10\n\t"
        "movq $0, (%r10)\n\t"
        "movq $7, 8(%r10)\n\t"
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
        "jmp *tlg_js_test_srv_gateway(%rip)\n\t");
}
typedef int (__attribute__((sysv_abi))* GuestFunction)(
    void*,void*,void*,void*,void*,void*);
static uint8_t* page(void) {
#ifdef _WIN32
    return (uint8_t*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
#else
    void* p=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                 MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return p==MAP_FAILED?NULL:p;
#endif
}
int main(void) {
    const size_t image_size=TLG_JS_100_CREATE_SRV_ENTRY+
                             TLG_JS_CREATE_SRV_FINGERPRINT_BYTES+1;
    uint8_t* image=calloc(image_size,1);
    assert(image && !tlg_js_100_srv_signature_match(image,image_size));
    memcpy(image+TLG_JS_100_CREATE_SRV_ENTRY,tlg_js_100_srv_fingerprint,
           TLG_JS_CREATE_SRV_FINGERPRINT_BYTES);
    assert(tlg_js_100_srv_signature_match(image,image_size));
    image[TLG_JS_100_CREATE_SRV_ENTRY+25]^=1;
    assert(!tlg_js_100_srv_signature_match(image,image_size));
    free(image);
    uint8_t* machine=page();
    assert(machine);
    static const uint8_t original[]={
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,
        0x48,0x02,0,0, /* sub rsp,0x248 */
        0xb8,0x2a,0,0,0, /* mov eax,42 */
        0x48,0x81,0xc4,0x48,0x02,0,0, /* add rsp,0x248 */
        0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3
    };
    memcpy(machine,original,sizeof(original));
    GuestFunction function=(GuestFunction)machine;
    assert(function((void*)0x101,(void*)0x102,(void*)0x103,
                    (void*)0x104,(void*)0x105,(void*)0x106)==42);
    uint8_t* trampoline=machine+128;
    assert(tlg_js_build_srv_trampoline(
        trampoline,4096-128,machine,TLG_JS_CREATE_SRV_PROLOGUE_BYTES,
        (uintptr_t)(machine+TLG_JS_CREATE_SRV_PROLOGUE_BYTES)) ==
        TLG_JS_CREATE_SRV_TRAMPOLINE_BYTES);
    tlg_js_test_srv_gateway=(uintptr_t)trampoline;
    tlg_js_write_absolute_jump(machine,(uintptr_t)hook);
    memset(machine+14,0x90,TLG_JS_CREATE_SRV_PROLOGUE_BYTES-14);
    __builtin___clear_cache((char*)machine,(char*)machine+64);
    assert(function((void*)0x101,(void*)0x102,(void*)0x103,
                    (void*)0x104,(void*)0x105,(void*)0x106)==42);
    assert(tlg_js_test_srv_calls==1);
    TlgJsNativeCommand raw={};
    TlgJsNativeSrvCreateCandidate srv={};
    uint64_t cursor=0,dropped=0;
    assert(tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&raw,&dropped));
    assert(tlg_js_native_decode_srv_create(&raw,&srv));
    assert(srv.sequence==1 && srv.device==0x101);
    assert(srv.input_rsi==0x102 && srv.input_rdx==0x103);
    assert(srv.input_rcx==0x104 && srv.input_r8==0x105 && srv.input_r9==0x106);
    assert(!tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&raw,&dropped));
    assert(dropped==0);
#ifdef _WIN32
    VirtualFree(machine,0,MEM_RELEASE);
#else
    munmap(machine,4096);
#endif
    puts("v01.00 CreateShaderResourceView signature, trampoline and ABI PASS");
    return 0;
}
