/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Supplement the original nine controls with nonzero-pending barriers.
 * Normal execution only: precise guest faults/async recovery are not tested. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
extern uint64_t pending_seg(uint64_t), pending_retimm(uint64_t);
extern uint64_t pending_poprsp(uint64_t), pending_pop16(uint64_t);
extern uint64_t pending_many254(uint64_t), pending_many256(uint64_t);
__asm__(
".text\n"
".global pending_seg\npending_seg:\npush %rbx\nmov %rdi,%rax\njmp 1f\nnop\n1:\npop %rbx\n.byte 0x2e,0xc3\n"
".global pending_retimm\npending_retimm:\npush %rbx\nmov %rdi,%rax\njmp 1f\nnop\n1:\npop %rbx\nret $16\n"
".global pending_poprsp\npending_poprsp:\npush %rbx\nmov %rsp,%r11\npush %r11\npush %r12\njmp 1f\nnop\n1:\npop %r12\npop %rsp\nmov %rdi,%rax\njmp 1f\nnop\n1:\npop %rbx\nret\n"
".global pending_pop16\npending_pop16:\npush %rbx\nsub $2,%rsp\nmovw $0x1234,(%rsp)\njmp 1f\nnop\n1:\npop %ax\nmov %rdi,%rax\njmp 1f\nnop\n1:\npop %rbx\nret\n"
/* Long POP sequences exercise TB splitting; exact signed12 boundaries
 * are tested by the extracted production predicate, not these guest paths. */
".global pending_many254\npending_many254:\n.rept 254\npush %r11\n.endr\nmov %rdi,%rax\n.rept 254\npop %r11\n.endr\nret\n"
".global pending_many256\npending_many256:\n.rept 256\npush %r11\n.endr\nmov %rdi,%rax\n.rept 256\npop %r11\n.endr\nret\n");
static uint64_t retimm(uint64_t x)
{
    uint64_t out;
    __asm__ volatile("sub $16,%%rsp; call pending_retimm"
                     : "=a"(out) : "D"(x)
                     : "rcx","rdx","rsi","r8","r9","r10","r11","cc","memory");
    return out;
}
int main(void)
{
    uint64_t seed=0x912345678abcdef0ULL, checksum=0;
    for (unsigned i=0;i<8192;i++) {
        seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
        if (pending_seg(seed)!=seed || retimm(seed)!=seed ||
            pending_poprsp(seed)!=seed || pending_pop16(seed)!=seed ||
            pending_many254(seed)!=seed || pending_many256(seed)!=seed) abort();
        checksum+=seed;
    }
    printf("RET_PENDING_PASS calls=49152 checksum=%llu\n",(unsigned long long)checksum);
    fflush(stdout);
    if (getpid()==1) for (;;) pause();
    return 0;
}
