/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _DUNE_STACK_H_
#define _DUNE_STACK_H_

#include "dune-kvm.h"

void dune_expand_stack(void);

void switch_host(struct dune_kvm_cpu *cpu);

void switch_stack(struct dune_kvm_cpu *cpu, uint64_t stack);

#endif
