/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_FAST_ITLB_H_
#define _LATX_FAST_ITLB_H_

#include "exec/translation-block.h"

typedef struct fast_itlb_entry {
    uint64_t mix; /* gva | gpa */
} fast_itlb_entry_t;

#define FAST_ITLB_BITS  10

typedef struct fast_itlb_table {
    fast_itlb_entry_t e[1 << FAST_ITLB_BITS];
} fast_itlb_table_t;

#define FAST_ITLB_SIZE  ((1ULL << FAST_ITLB_BITS) << 3)

#define FAST_ITLB_MASK  ((1 << FAST_ITLB_BITS) - 1)

#define FAST_ITLB_INDEX(a)  (((a)>>12) & FAST_ITLB_MASK)

void *cpu_fast_itlb_base(CPUState *cpu, int id);

void cpu_fast_itlb_flush(CPUState *cpu, int mmuidx);
void cpu_fast_itlb_flush_ptr(void *itlb);
void cpu_fast_itlb_flush_page(CPUState *cpu, uint64_t page, int mmuidx);

void cpu_fast_itlb_fill(CPUState *cpu, int mmuidx,
        uint64_t gva, uint64_t gpa, int prot);
#endif
