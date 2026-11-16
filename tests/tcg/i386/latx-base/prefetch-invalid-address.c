/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    long size = sysconf(_SC_PAGESIZE);
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return 2;
    }
    uintptr_t addresses[] = {0, 1, (uintptr_t)p, (uintptr_t)p + size - 1,
                            UINT64_C(0x800000000000), UINT64_MAX};
    for (unsigned i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        __asm__ volatile("prefetcht0 (%0)" : : "r"(addresses[i]));
    }
    munmap(p, size);
    puts("PREFETCH_INVALID_COUNT=6 PASSED");
    return 0;
}
