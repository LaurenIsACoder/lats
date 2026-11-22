/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdint.h>
#if __STDC_HOSTED__
#include <stdio.h>
#else
#include <minilib.h>
#define printf ml_printf
#endif

typedef struct Pair {
    double low;
    uint64_t high;
} Pair;

static __attribute__((noinline)) void scale(Pair *out, const double *value,
                                            const Pair *factor)
{
    /*
     * A memory MOVSD clears bits 127:64. MULSD preserves those bits and
     * MOVUPD makes both lanes observable.
     */
    __asm__ volatile (
        "movupd (%[factor]), %%xmm1\n\t"
        "movapd %%xmm1, %%xmm0\n\t"
        "movsd (%[value]), %%xmm0\n\t"
        "mulsd %%xmm1, %%xmm0\n\t"
        "movupd %%xmm0, (%[out])"
        :
        : [factor] "r" (factor), [value] "r" (value), [out] "r" (out)
        : "xmm0", "xmm1", "memory");
}

int main(void)
{
    const Pair factor = { 3.0, UINT64_C(0x4000000000000000) };
    unsigned failures = 0;

    for (unsigned i = 1; i <= 24; i++) {
        double input = i;
        Pair result;

        scale(&result, &input, &factor);
        if (result.low != input * factor.low || result.high != 0) {
            if (!failures) {
                printf("first mismatch at input %u, high bits %llx\n",
                       i, (unsigned long long)result.high);
            }
            failures++;
        }
    }
    printf("SCALAR_LANES total=24 failures=%u\n", failures);
    return failures != 0;
}
