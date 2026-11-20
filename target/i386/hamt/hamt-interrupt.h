/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HATM_INTERRUPT_H_
#define _HATM_INTERRUPT_H_

#include "hamt-cpu.h"

extern uint64_t hamt_int_ipi_entry_begin[];
extern uint64_t hamt_int_ipi_entry_end[];
extern int      hamt_int_ipi_entry_size[];

extern uint64_t hamt_ipi_data_area[];

void hamt_prepare_ipi(int cpuid, int n);
void hamt_send_ipi(int cpuid, void *target_cpu);
int hamt_finish_ipi(int cpuid, uint64_t *r, int n, int mask, int waitcpus);

void hamt_enable_local_ipi(void);

extern uint64_t hamt_int_ti_entry_begin[HAMT_CPU_MAX];
extern uint64_t hamt_int_ti_entry_end[HAMT_CPU_MAX];
extern int      hamt_int_ti_entry_size[HAMT_CPU_MAX];
#endif
