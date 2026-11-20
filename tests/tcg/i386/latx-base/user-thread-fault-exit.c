/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#define THREADS 8
#define ITERATIONS 2000
#define SSE_LANES 2

struct fault_output {
    uint64_t sse[SSE_LANES];
    uint64_t r14;
    uint64_t r15;
};

extern char fault_insn[], resume_insn[];
extern void fault_with_sse(volatile struct fault_output *output,
                           const void *fault_addr, uint64_t marker,
                           uint64_t base);

/*
 * rdi: output, rsi: PROT_NONE address, rdx: expected R15, rcx: lane base.
 * The external assembly keeps the faulting store, resume PC, SSE state, and
 * general registers visible across signal delivery and sigreturn.
 */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl fault_with_sse\n"
    ".type fault_with_sse, @function\n"
    "fault_with_sse:\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq %rdx, %r15\n"
    "    movq %rcx, %r14\n"
    "    movq %rcx, -24(%rsp)\n"
    "    movq %rcx, %rax\n"
    "    movabsq $0x1111111111111111, %r8\n"
    "    xorq %r8, %rax\n"
    "    movq %rax, -16(%rsp)\n"
    "    movdqu -24(%rsp), %xmm1\n"
    "    paddq %xmm1, %xmm1\n"
    ".globl fault_insn\n"
    ".type fault_insn, @function\n"
    "fault_insn:\n"
    "    movq %r15, (%rsi)\n"
    ".globl resume_insn\n"
    ".type resume_insn, @function\n"
    "resume_insn:\n"
    "    movdqu %xmm1, (%rdi)\n"
    "    movq %r14, 16(%rdi)\n"
    "    movq %r15, 24(%rdi)\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    retq\n"
    ".size fault_with_sse, .-fault_with_sse\n"
);

static __thread volatile sig_atomic_t handler_count;
static __thread volatile sig_atomic_t recovery_count;
static __thread volatile sig_atomic_t completed_iterations;
static __thread volatile sig_atomic_t worker_failed;
static __thread volatile struct fault_output output;
static sig_atomic_t handler_counts[THREADS];
static sig_atomic_t recovery_counts[THREADS];
static sig_atomic_t completed[THREADS];
static uint8_t *fault_page;

static void handle_segv(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;

    if (sig != SIGSEGV || info == NULL || uc == NULL ||
        uc->uc_mcontext.gregs[REG_RIP] != (greg_t)fault_insn ||
        info->si_addr != (void *)uc->uc_mcontext.gregs[REG_RSI]) {
        _exit(128 + SIGSEGV);
    }

    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)resume_insn;
    handler_count++;
}

static void *worker(void *opaque)
{
    uintptr_t id = (uintptr_t)opaque;

    for (volatile unsigned i = 0; i < ITERATIONS; i++) {
        uint64_t base = ((uint64_t)id << 32) ^ i;
        uint64_t expected[SSE_LANES] = {
            base,
            base ^ UINT64_C(0x1111111111111111),
        };
        uint64_t marker = UINT64_C(0x9e3779b97f4a7c15) ^
                          ((uint64_t)id << 32) ^ i;

        for (unsigned lane = 0; lane < SSE_LANES; lane++) {
            expected[lane] *= 2;
            output.sse[lane] = 0;
        }
        output.r14 = 0;
        output.r15 = 0;

        fault_with_sse(&output, fault_page, marker, base);
        recovery_count++;

        if (handler_count != (sig_atomic_t)(i + 1) ||
            recovery_count != (sig_atomic_t)(i + 1)) {
            worker_failed = 1;
            break;
        }
        for (unsigned lane = 0; lane < SSE_LANES; lane++) {
            if (output.sse[lane] != expected[lane]) {
                worker_failed = 1;
            }
        }
        if (output.r14 != base || output.r15 != marker) {
            worker_failed = 1;
        }
        if (worker_failed) {
            break;
        }

        completed_iterations = (sig_atomic_t)(i + 1);
    }

    handler_counts[id] = handler_count;
    recovery_counts[id] = recovery_count;
    completed[id] = completed_iterations;
    return worker_failed ? (void *)1 : NULL;
}

int main(void)
{
    struct sigaction action = {0};
    pthread_t threads[THREADS];
    unsigned created = 0;
    unsigned long signals = 0;
    unsigned long recoveries = 0;
    unsigned long completed_iterations = 0;
    unsigned failed = 0;
    long page_size = sysconf(_SC_PAGESIZE);

    action.sa_sigaction = handle_segv;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (page_size <= 0) {
        return 2;
    }

    fault_page = mmap(NULL, (size_t)page_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fault_page == MAP_FAILED || sigaction(SIGSEGV, &action, NULL) != 0) {
        return 2;
    }

    for (uintptr_t i = 0; i < THREADS; i++) {
        if (pthread_create(&threads[i], NULL, worker, (void *)i) != 0) {
            failed++;
            break;
        }
        created++;
    }
    for (unsigned i = 0; i < created; i++) {
        void *result = NULL;
        if (pthread_join(threads[i], &result) != 0 || result != NULL) {
            failed++;
        }
    }

    for (unsigned i = 0; i < THREADS; i++) {
        signals += (unsigned long)handler_counts[i];
        recoveries += (unsigned long)recovery_counts[i];
        completed_iterations += (unsigned long)completed[i];
        if (handler_counts[i] != ITERATIONS ||
            recovery_counts[i] != ITERATIONS ||
            completed[i] != ITERATIONS) {
            failed++;
        }
    }

    munmap(fault_page, (size_t)page_size);
    printf("THREADS=%u ITERATIONS=%u SIGNALS=%lu RECOVERIES=%lu "
           "COMPLETED=%lu FAILED=%u\n",
           THREADS, ITERATIONS, signals, recoveries, completed_iterations,
           failed);
    return failed != 0 || signals != THREADS * ITERATIONS ||
           recoveries != THREADS * ITERATIONS ||
           completed_iterations != THREADS * ITERATIONS;
}
