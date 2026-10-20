/* SPDX-License-Identifier: GPL-2.0-or-later
 * Standalone executable test of the native Js draw-capture ABI.
 * No game files, GPU, shaders, or drivers required.
 */
#define _GNU_SOURCE
#include "../src/native_js_commands.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

__attribute__((used,aligned(64))) TlgJsNativeRing tlg_js_native_ring;
__attribute__((used)) uintptr_t tlg_js_test_gateway;

/* Same register-preserving assembly as the production indexed hook.
 * Changing this requires updating both copies and rerunning the test.
 */
__attribute__((naked,used,noinline))
static void test_hook(void) {
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
        "movq $1, 8(%r10)\n\t"
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
        "jmp *tlg_js_test_gateway(%rip)\n\t"
    );
}
static void jump(uint8_t *at, uintptr_t destination) {
    static const uint8_t op[] = {0xff,0x25,0,0,0,0};
    memcpy(at,op,6);
    memcpy(at+6,&destination,8);
}
typedef int (__attribute__((sysv_abi)) *GuestFn)(void*,void*,void*,void*,void*,void*);
int main(void) {
#ifdef _WIN32
    uint8_t *machine = (uint8_t*)VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
#else
    uint8_t *machine = mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                             MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(machine==MAP_FAILED)machine=NULL;
#endif
    assert(machine);
    const uint8_t orig[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x89,0xfb,
        0x5b,0x41,0x5c,0x41,0x5e,0x41,0x5f,0x5d,0xb8,0x2a,0,0,0,0xc3
    };
    memcpy(machine,orig,sizeof(orig));
    memcpy(machine+128,orig,14);
    jump(machine+128+14,(uintptr_t)(machine+14));
    tlg_js_test_gateway=(uintptr_t)(machine+128);
    jump(machine,(uintptr_t)test_hook);
    __builtin___clear_cache((char*)machine,(char*)machine+192);
    const GuestFn fn=(GuestFn)machine;
    for(unsigned i=0;i<1000;i++) {
        assert(fn((void*)0xabcdef,(void*)(uintptr_t)(i+1),(void*)0x12,
                  (void*)0x34,(void*)0x56,(void*)0x78)==42);
    }
    assert(tlg_js_native_ring.head==1000);
    uint64_t cursor=0,dropped=0;
    TlgJsNativeCommand command;
    TlgJsNativeDrawCandidate decoded;
    for(unsigned i=0;i<1000;i++) {
        assert(tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&command,&dropped));
        assert(command.ready==i+1);
        assert(command.context==0xabcdef);
        assert(command.kind==TLG_JS_NATIVE_INDEXED);
        assert(command.arg0==i+1);
        assert(command.arg1==0x12 && command.arg2==0x34 &&
               command.arg3==0x56 && command.arg4==0x78);
        assert(tlg_js_native_decode_draw(&command,&decoded));
        assert(decoded.index_count==i+1 && decoded.instance_count==1);
        assert(decoded.first_index==0x12);
    }
    assert(!tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&command,&dropped));
    assert(dropped==0);
    for(unsigned i=0;i<TLG_JS_NATIVE_RING_CAPACITY+200;i++) {
        assert(fn((void*)0xabcdef,(void*)(uintptr_t)i,(void*)0x12,
                  (void*)0x34,(void*)0x56,(void*)0x78)==42);
    }
    unsigned read=0;
    while(tlg_js_native_read_next(&tlg_js_native_ring,&cursor,&command,&dropped))
        ++read;
    assert(read==TLG_JS_NATIVE_RING_CAPACITY);
    assert(dropped==200);

    /* A valid instanced record maps indexCount, instanceCount, firstIndex. */
    memset(&command,0,sizeof(command));
    command.ready=88;
    command.kind=TLG_JS_NATIVE_INSTANCED;
    command.context=0xdeadbeef;
    command.arg0=450;
    command.arg1=7;
    command.arg2=15;
    assert(tlg_js_native_decode_draw(&command,&decoded));
    assert(decoded.kind==TLG_JS_NATIVE_INSTANCED);
    assert(decoded.index_count==450 && decoded.instance_count==7 && decoded.first_index==15);
    command.kind=999;
    assert(!tlg_js_native_decode_draw(&command,&decoded));
#ifdef _WIN32
    VirtualFree(machine,0,MEM_RELEASE);
#else
    munmap(machine,4096);
#endif
    printf("Native Js draw capture PASS: 4096 snapshots and overflow recovery\n");
    return 0;
}
