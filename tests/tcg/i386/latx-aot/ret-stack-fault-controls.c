/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <string.h>

extern uint64_t ret_stack_fault(void *);
extern char ret_stack_fault_pc[], ret_stack_resume[];
__asm__(
".text\n.global ret_stack_fault\nret_stack_fault:\n"
"push %rbx\npush %r12\nmov %rsp,%r12\nmov %rdi,%rsp\njmp 1f\nnop\n"
"1: stc\npop %rbx\n"
".global ret_stack_fault_pc\nret_stack_fault_pc: ret\n"
".global ret_stack_resume\nret_stack_resume:\nsetc %al\nmovzbl %al,%eax\n"
"mov %r12,%rsp\npop %r12\npop %rbx\nret\n");
static unsigned char signal_stack[65536];
static unsigned char *guard_page;
static size_t page_size;
static uintptr_t expected_rsp;
static volatile sig_atomic_t faults;
static volatile sig_atomic_t target_fetch;
static const uint64_t saved_rbx=UINT64_C(0x9876543210abcdef);
static void fault_handler(int sig, siginfo_t *info, void *opaque)
{
    ucontext_t *uc=opaque;
    uintptr_t fault_addr=(uintptr_t)info->si_addr;
    if (sig!=SIGSEGV || fault_addr<(uintptr_t)guard_page ||
        fault_addr>=(uintptr_t)guard_page+page_size ||
        uc->uc_mcontext.gregs[REG_RIP]!=(greg_t)
            (target_fetch ? (uintptr_t)guard_page : (uintptr_t)ret_stack_fault_pc) ||
        uc->uc_mcontext.gregs[REG_RSP]!=(greg_t)(expected_rsp+target_fetch*8) ||
        (uint64_t)uc->uc_mcontext.gregs[REG_RBX]!=saved_rbx ||
        !(uc->uc_mcontext.gregs[REG_EFL]&1)) _exit(41);
    if (target_fetch) uc->uc_mcontext.gregs[REG_RIP]=(greg_t)(uintptr_t)ret_stack_resume;
    else if (mprotect(guard_page,page_size,PROT_READ|PROT_WRITE)) _exit(42);
    faults++;
}
int main(void)
{
    page_size=sysconf(_SC_PAGESIZE);
    unsigned char *mem=mmap(0,page_size*3,PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (mem==MAP_FAILED) abort();
    guard_page=mem+page_size;
    stack_t ss={.ss_sp=signal_stack,.ss_size=sizeof(signal_stack)};
    struct sigaction sa={.sa_sigaction=fault_handler,.sa_flags=SA_SIGINFO|SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    if (sigaltstack(&ss,0) || sigaction(SIGSEGV,&sa,0)) abort();
    for (unsigned i=0;i<2048;i++) {
        expected_rsp=(uintptr_t)guard_page-(i%2?4:0);
        uintptr_t target=(uintptr_t)ret_stack_resume;
        memcpy((void *)(expected_rsp-8),&saved_rbx,8);
        memcpy((void *)expected_rsp,&target,8);
        if (mprotect(guard_page,page_size,PROT_NONE)) abort();
        if (ret_stack_fault((void *)(expected_rsp-8))!=1 || faults!=(sig_atomic_t)(i+1)) abort();
    }
    /* A target-fetch fault observes the already completed RET pop. */
    target_fetch=1;
    expected_rsp=(uintptr_t)(mem+page_size*2+16);
    uintptr_t target=(uintptr_t)guard_page;
    memcpy((void *)(expected_rsp-8),&saved_rbx,8);
    memcpy((void *)expected_rsp,&target,8);
    if (mprotect(guard_page,page_size,PROT_NONE)) abort();
    for (unsigned i=0;i<32;i++) {
        if (ret_stack_fault((void *)(expected_rsp-8))!=1 ||
            faults!=(sig_atomic_t)(2049+i)) abort();
    }
    printf("RET_STACK_FAULT_PASS faults=%d aligned=1024 crosspage=1024 target_fetch=32 flags=CF\n",(int)faults);
    fflush(stdout);
    if (getpid()==1) for (;;) pause();
    return 0;
}
