/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _DUNE_KVM_INIT_H_
#define _DUNE_KVM_INIT_H_

void dune_kvm_init_ebase(struct dune_kvm_cpu *cpu, int mode, int cpuid);
void dune_kvm_init_csr(struct dune_kvm_cpu *cpu, int mode, int cpuid);
void dune_kvm_init_fpu(struct dune_kvm_cpu *cpu, int mode, int cpuid);

#endif
