/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_HAMT_H_
#define _LATX_HAMT_H_

void *latx_dump_hamt_context(void);

#include "hamt-options.h"
void hamt_init_options_configuration(void);
uint64_t hamt_kvm_cpu_read_pc(void *cpu);

#endif
