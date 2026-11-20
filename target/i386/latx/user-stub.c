/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <sys/syscall.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <fenv.h>
#include <math.h>
#include "common.h"
#include "lsenv.h"
#include "latx-config.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "qemu/cacheflush.h"
#include "accel/tcg/internal.h"
#include "loongarch-extcontext.h"
#include "latx-sigrt-sys.h"
#include "latx-ras.h"
#include "latx-hamt.h"

#ifndef CONFIG_SOFTMMU

void lats_send_sigrtmin(CPUState *cpu)
{
    return;
}

void cpu_loop_exit_check_ptn_eflag(CPUState *cpu, uintptr_t epc);
void cpu_loop_exit_check_ptn_eflag(CPUState *cpu, uintptr_t epc)
{
    return;
}

#endif
