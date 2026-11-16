/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "kernels.h"
#define NOINLINE __attribute__((noinline))
NOINLINE static uint64_t callee(uint64_t x) { return ((x << 7) ^ (x >> 3)) + 17; }
static uint64_t (*volatile target)(uint64_t) = callee;
void prepare_data(void) {}

#if defined(__x86_64__)
/* Explicit forms keep the memory operand from becoming a register CALL. */
#define CLOBBERS "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "cc", "memory", \
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", \
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
NOINLINE static uint64_t call_memory(uint64_t n, uint64_t x) {
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t out;
        __asm__ volatile("call *%2" : "=&a"(out), "+D"(x) : "m"(target) : CLOBBERS);
        x = out;
    }
    return x;
}
#define STACK_CALL(name, displacement) \
NOINLINE static uint64_t name(uint64_t n, uint64_t x) { \
    for (uint64_t i = 0; i < n; ++i) { \
        uint64_t out; \
        __asm__ volatile("sub $16, %%rsp\n\tmov %2, " displacement "(%%rsp)\n\t" \
            "call *" displacement "(%%rsp)\n\tadd $16, %%rsp" \
            : "=&a"(out), "+D"(x) : "r"(target) : CLOBBERS); \
        x = out; \
    } \
    return x; \
}
STACK_CALL(call_rsp, "8")
/* CALL overwrites this slot: its target must be read before pushing return PC. */
STACK_CALL(call_rsp_overwritten, "-8")
#else
NOINLINE static uint64_t reference(uint64_t n, uint64_t x) {
    for (uint64_t i = 0; i < n; ++i) x = target(x);
    return x;
}
#define call_memory reference
#define call_rsp reference
#define call_rsp_overwritten reference
#endif
const struct kernel kernels[] = {
    {"call_memory", call_memory}, {"call_rsp", call_rsp},
    {"call_rsp_overwritten", call_rsp_overwritten}
};
const size_t kernel_count = sizeof(kernels) / sizeof(kernels[0]);
