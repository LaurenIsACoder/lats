/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_INIT_H_
#define _HAMT_INIT_H_

extern uint64_t hamt_int_data_area[];

void hamt_init_kvm_cpu(void *cpu, void *kvmcpu);
void hamt_init_kvm_regs(void *cpu, int size);

void hamt_prepare(int mode, int cpuid);

#endif
