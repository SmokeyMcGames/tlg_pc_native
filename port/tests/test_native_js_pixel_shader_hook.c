/* SPDX-License-Identifier: GPL-2.0-or-later
 * v01.00 JsDevice::CreatePixelShader high-level API capture.
 * Synthetic x86-64 code, no proprietary game data or Vulkan driver.
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
__attribute__((used)) uintptr_t tlg_js_test_pixel_gateway;

__attribute__((naked, used, noinline))
static void test_pixel_entry(void) {
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
        "movq $4, 8(%r10)\n\t"
        "movq %rdi, 16(%r10)\n\t"
        "movq %rsi, 24(%r10)\n\t"
        "movq %rdx, 32(%r10)\n\t"
        "movq %rcx, 40(%r10)\n\t"
        "movq $0, 48(%r10)\n\t"
        "movq $0, 56(%r10)\n\t"
        "testq %rsi, %rsi\n\t"
        "jz 1f\n\t"
        "movq (%rsi), %rax\n\t"
        "movq %rax, 48(%r10)\n\t"
        "movq 8(%rsi), %rax\n\t"
        "movq %rax, 56(%r10)\n\t"
        "1:\n\t"
        "movq %r11, (%r10)\n\t"
        "popq %r11\n\t"
        "popq %r10\n\t"
        "popq %rax\n\t"
        "popfq\n\t"
        "jmp *tlg_js_test_pixel_gateway(%rip)\n\t");
}
typedef int (__attribute__((sysv_abi)) *Function)(void*,void*,void*,void*);

static uint8_t* executable_page(void) {
#ifdef _WIN32
    return (uint8_t*)VirtualAlloc(NULL, 4096, MEM_RESERVE|MEM_COMMIT,
                                   PAGE_EXECUTE_READWRITE);
#else
    void *p=mmap(NULL, 4096, PROT_READ|PROT_WRITE|PROT_EXEC,
                 MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    return p==MAP_FAILED ? NULL : (uint8_t*)p;
#endif
}
int main(void) {
    /* Signature verification must reject mismatched versions before writing. */
    const size_t size=TLG_JS_100_CREATE_PIXEL_SHADER_ENTRY+
                      TLG_JS_CREATE_PIXEL_SHADER_FINGERPRINT_BYTES + 1;
    uint8_t *synthetic_elf=(uint8_t*)calloc(size,1);
    assert(synthetic_elf);
    assert(!tlg_js_100_pixel_shader_signature_match(synthetic_elf,size));
    memcpy(synthetic_elf+TLG_JS_100_CREATE_PIXEL_SHADER_ENTRY,
           tlg_js_100_create_pixel_shader_fingerprint,
           TLG_JS_CREATE_PIXEL_SHADER_FINGERPRINT_BYTES);
    assert(tlg_js_100_pixel_shader_signature_match(synthetic_elf,size));
    synthetic_elf[TLG_JS_100_CREATE_PIXEL_SHADER_ENTRY+23]^=1;
    assert(!tlg_js_100_pixel_shader_signature_match(synthetic_elf,size));
    free(synthetic_elf);

    uint8_t *machine=executable_page();
    assert(machine);
    static const uint8_t synthetic_function[]={
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,
        0x38,0x02,0,0,
        0xb8,42,0,0,0, /* mov eax,42 */
        0x48,0x81,0xc4,0x38,0x02,0,0, /* add rsp,0x238 */
        0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3
    };
    memcpy(machine,synthetic_function,sizeof(synthetic_function));
    Function fn=(Function)machine;
    uint64_t shader_blob[2]={0x1122334455667788ull,0x99aabbccddeeff00ull};
    assert(fn((void*)0xa001,shader_blob,(void*)0xb002,(void*)0xc003)==42);
    uint8_t *trampoline=machine+128;
    assert(tlg_js_build_create_buffer_trampoline(
        trampoline,4096-128,machine,TLG_JS_CREATE_BUFFER_PROLOGUE_BYTES,
        (uintptr_t)(machine+TLG_JS_CREATE_BUFFER_PROLOGUE_BYTES))==
        TLG_JS_CREATE_BUFFER_TRAMPOLINE_BYTES);
    tlg_js_test_pixel_gateway=(uintptr_t)trampoline;
    tlg_js_write_absolute_jump(machine,(uintptr_t)&test_pixel_entry);
    memset(machine+14,0x90,TLG_JS_CREATE_BUFFER_PROLOGUE_BYTES-14);
    __builtin___clear_cache((char*)machine,(char*)machine+64);
    assert(fn((void*)0xa001,shader_blob,(void*)0xb002,(void*)0xc003)==42);
    assert(fn((void*)0xa005,shader_blob,(void*)0xb006,(void*)0xc007)==42);
    uint64_t cursor=0,dropped=0;
    TlgJsNativeCommand captured={};
    TlgJsNativeShaderCreateCandidate shader={};
    for(int i=0;i<2;i++) {
        assert(tlg_js_native_read_next(&tlg_js_native_ring,&cursor,
                                        &captured,&dropped));
        assert(tlg_js_native_decode_pixel_shader_create(&captured,&shader));
        assert(shader.device==(i==0 ? 0xa001u : 0xa005u));
        assert(shader.shader_blob==(uintptr_t)shader_blob);
        assert(shader.input_rdx==(i==0 ? 0xb002u : 0xb006u));
        assert(shader.output_rcx==(i==0 ? 0xc003u : 0xc007u));
        assert(shader.blob_head==shader_blob[0]);
        assert(shader.blob_tail==shader_blob[1]);
    }
    assert(!tlg_js_native_read_next(&tlg_js_native_ring,&cursor,
                                     &captured,&dropped));
    assert(dropped==0);
#ifdef _WIN32
    VirtualFree(machine,0,MEM_RELEASE);
#else
    munmap(machine,4096);
#endif
    puts("v01.00 JsDevice CreatePixelShader hook, trampoline, header snapshot PASS");
    return 0;
}
