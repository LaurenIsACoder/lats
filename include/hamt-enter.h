/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HATM_ENTER_H_
#define _HATM_ENTER_H_

#include "hamt-options.h"

int dune_enter(void *cpu, int mode, int cpuid);

#define HAMT_MODE_RR    1
#define HAMT_MODE_MT    2

static inline int cpu_enter_hamt(void *cpu, int mode, int cpuid)
{
    if (hamt_enabled()) {
        return dune_enter(cpu, mode, cpuid);
    } else {
        return 0;
    }
}

#endif
