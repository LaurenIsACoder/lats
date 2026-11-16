/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Experimental RSP folding: data/stack preservation and flush barriers. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
extern uint64_t fold_pops(uint64_t), fold_cancel(uint64_t), fold_mem(uint64_t), fold_flags(uint64_t), fold_prefix(uint64_t), fold_retimm(uint64_t), fold_enter(uint64_t), fold_large(uint64_t), fold_edge(uint64_t);
__asm__(
".text\n"
".global fold_pops\nfold_pops:\n"
"push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\nsub $8,%rsp\n"
"mov %rdi,%rax\nadd $8,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n"
".global fold_cancel\nfold_cancel:\n"
"push %rbx\nsub $8,%rsp\nadd $8,%rsp\nsub $8,%rsp\nadd $8,%rsp\nmov %rdi,%rax\npop %rbx\nret\n"
".global fold_mem\nfold_mem:\nsub $32,%rsp\nmov %rdi,8(%rsp)\nadd $8,%rsp\nmov (%rsp),%rax\nadd $24,%rsp\nret\n"
".global fold_flags\nfold_flags:\nsub $32,%rsp\nadd $8,%rsp\nsetc %dl\nmovzbl %dl,%eax\nadd $24,%rsp\nret\n"
".global fold_prefix\nfold_prefix:\npush %rbx\nmov %rdi,%rax\npop %rbx\n.byte 0xf3\nret\n"
".global fold_enter\nfold_enter:\npush %rbp\nsub $96,%rsp\nlea 64(%rsp),%rbp\nmov %rdi,-8(%rbp)\nmov %rdi,%rax\nnot %rax\nmov %rax,-40(%rbp)\nsub $32,%rsp\nenter $16,$2\nmov -8(%rbp),%rax\nleave\nadd $32,%rsp\nadd $96,%rsp\npop %rbp\nret\n"
".global fold_large\nfold_large:\npush %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\nsub $2024,%rsp\nmov %rdi,(%rsp)\nmov %rdi,%rax\nadd $2024,%rsp\npop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n"
".global fold_edge\nfold_edge:\npush %rbx\nsub $2024,%rsp\nmov %rdi,(%rsp)\nmov %rdi,%rax\nadd $2024,%rsp\npop %rbx\nret\n"
".global fold_retimm\nfold_retimm:\nmov %rdi,%rax\nret $16\n");
static uint64_t retimm(uint64_t x) {
 uint64_t out;
 __asm__ volatile("sub $16,%%rsp; call fold_retimm" : "=a"(out) : "D"(x) : "rcx","rdx","rsi","r8","r9","r10","r11","cc","memory");
 return out;
}
int main(void) {
 uint64_t seed=0x912345678abcdef0ULL, checksum=0;
 for(unsigned i=0;i<196608;i++) {
  seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
  if(fold_pops(seed)!=seed || fold_cancel(seed)!=seed || fold_mem(seed)!=seed || fold_flags(seed)!=0 || fold_prefix(seed)!=seed || retimm(seed)!=seed || fold_enter(seed)!=seed || fold_large(seed)!=seed || fold_edge(seed)!=seed) abort();
  checksum+=seed;
 }
 printf("STACK_RET_PASS calls=1769472 checksum=%llu\n",(unsigned long long)checksum);fflush(stdout);
 if(getpid()==1)for(;;)pause();
 return 0;
}
