/* SPDX-License-Identifier: GPL-2.0-or-later
 * Tests the v01.00 JsDevice::CreateBuffer entry hook and its 20-byte trampoline
 * using synthetic machine code, not copyrighted game code.
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

__attribute__((used, aligned(64))) TlgJsNativeRing tlg_js_native_ring;
__attribute__((used)) uintptr_t tlg_js_test_create_buffer_gateway;
__attribute__((used)) volatile uint64_t tlg_js_test_create_buffer_calls;

__attribute__((naked,used,noinline))
static void hook_entry(void) {
    __asm__ volatile(
        "pushfq\n\t"
        "pushq %rax\n\t"
        "pushq %r10\n\t"
        "pushq %r11\n\t"
        "lock incq tlg_js_test_create_buffer_calls(%rip)\n\t"
        "leaq tlg_js_native_ring(%rip), %r10\n\t"
        "movq $1, %rax\n\t"
        "lock xaddq %rax, (%r10)\n\t"
        "leaq 1(%rax), %r11\n\t"
        "andq $4095, %rax\n\t"
        "shlq $6, %rax\n\t"
        "addq $64, %r10\n\t"
        "addq %rax, %r10\n\t"
        "movq $0, (%r10)\n\t"
        "movq $3, 8(%r10)\n\t"
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
        "jmp *tlg_js_test_create_buffer_gateway(%rip)\n\t");
}
typedef int (__attribute__((sysv_abi)) *GuestFunction)(
    void*, void*, void*, void*, void*, void*);

static uint8_t* alloc_exec(void) {
#ifdef _WIN32
    return (uint8_t*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
#else
    void *mem=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                   MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return mem==MAP_FAILED ? NULL : (uint8_t*)mem;
#endif
}

int main(void) {
    const size_t n = TLG_JS_100_CREATE_BUFFER_ENTRY +
                     TLG_JS_CREATE_BUFFER_FINGERPRINT_BYTES + 1;
    uint8_t *image = calloc(n,1);
    assert(image);
    assert(!tlg_js_100_create_buffer_signature_match(image,n));
    memcpy(image+TLG_JS_100_CREATE_BUFFER_ENTRY,
           tlg_js_100_create_buffer_fingerprint,
           TLG_JS_CREATE_BUFFER_FINGERPRINT_BYTES);
    assert(tlg_js_100_create_buffer_signature_match(image,n));
    image[TLG_JS_100_CREATE_BUFFER_ENTRY+23]^=1;
    assert(!tlg_js_100_create_buffer_signature_match(image,n));
    free(image);

    uint8_t *machine=alloc_exec();
    assert(machine);
    // mov eax,42 after the original prologue, then undo prologue and return.
    static const uint8_t synthetic[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,
        0x38,0x02,0x00,0x00,
        0xb8,0x2a,0,0,0,                  /* mov eax, 42 */
        0x48,0x81,0xc4,0x38,0x02,0,0,   /* add rsp, 0x238 */
        0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3
    };
    memcpy(machine,synthetic,sizeof(synthetic));
    GuestFunction function=(GuestFunction)machine;
    assert(function((void*)0xabc0,(void*)0x1000,(void*)0x2000,
                    (void*)0x3000,(void*)0x4000,(void*)0x5000)==42);

    uint8_t *trampoline=machine+128;
    assert(tlg_js_build_create_buffer_trampoline(
        trampoline,4096-128,machine,TLG_JS_CREATE_BUFFER_PROLOGUE_BYTES,
        (uintptr_t)(machine+TLG_JS_CREATE_BUFFER_PROLOGUE_BYTES)) ==
        TLG_JS_CREATE_BUFFER_TRAMPOLINE_BYTES);
    tlg_js_test_create_buffer_gateway=(uintptr_t)trampoline;
    tlg_js_write_absolute_jump(machine,(uintptr_t)hook_entry);
    memset(machine+14,0x90,TLG_JS_CREATE_BUFFER_PROLOGUE_BYTES-14);
    __builtin___clear_cache((char*)machine,(char*)machine+64);
    assert(function((void*)0xabc0,(void*)0x1000,(void*)0x2000,
                    (void*)0x3000,(void*)0x4000,(void*)0x5000)==42);
    assert(function((void*)0xabc1,(void*)0x1001,(void*)0x2001,
                    (void*)0x3001,(void*)0x4001,(void*)0x5001)==42);
    assert(tlg_js_test_create_buffer_calls==2);

    uint64_t cursor=0,dropped=0;
    TlgJsNativeCommand cmd;
    TlgJsNativeBufferCreateCandidate created;
    assert(tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&cmd,&dropped));
    assert(tlg_js_native_decode_buffer_create(&cmd,&created));
    assert(created.device==0xabc0 && created.description==0x1000);
    assert(created.initial_data==0x2000 && created.output_object==0x3000);
    assert(tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&cmd,&dropped));
    assert(tlg_js_native_decode_buffer_create(&cmd,&created));
    assert(created.device==0xabc1 && created.description==0x1001);
    assert(created.initial_data==0x2001 && created.output_object==0x3001);
    assert(!tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&cmd,&dropped));
    assert(dropped==0);
    cmd.kind=TLG_JS_NATIVE_INDEXED;
    assert(!tlg_js_native_decode_buffer_create(&cmd,&created));
#ifdef _WIN32
    VirtualFree(machine,0,MEM_RELEASE);
#else
    munmap(machine,4096);
#endif
    puts("v01.00 JsDevice CreateBuffer entry + semantic capture PASS");
    return 0;
}
