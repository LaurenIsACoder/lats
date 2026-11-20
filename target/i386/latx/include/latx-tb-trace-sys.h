/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_TB_TRACE_H_
#define _LATX_TB_TRACE_H_

#include "qemu/osdep.h"

#define LATXS_TB_TRACE_ENABLE

#ifdef CONFIG_LATX_RR
#define LATXS_TB_TRACE_ENABLE
#endif

#ifdef LATXS_TB_TRACE_ENABLE

#define latxs_tb_trace(env, tb, isexcp) do {    \
      __latxs_tb_trace(env, tb, isexcp);        \
} while (0)

#define latxs_tb_trace_nocheck(env, tb, config, isexcp) do { \
      __latxs_tb_trace_nocheck(env, tb, NULL, config, isexcp);     \
} while (0)

void __latxs_tb_trace(CPUX86State *env, TranslationBlock *tb, int isexcp);
void __latxs_tb_trace_nocheck(CPUX86State *env, TranslationBlock *tb,
        FILE *fd, int level, int isexcp);

#define latxs_tb_trace_rr(env, tb, isexcp) do { \
      __latxs_tb_trace_rr(env, tb, isexcp);     \
} while (0)

void __latxs_tb_trace_rr(CPUX86State *env, TranslationBlock *tb, int isexcp);
void __latxs_tb_trace_set_rr(int, int, int, int, int);

#else /* disable simple trace */

#define latxs_tb_trace(env, tb, isexcp)
#define latxs_tb_trace_nocheck(env, tb, level, isexcp)
#define latxs_tb_trace_rr(env, tb, isexcp)

#endif /* LATXS_TB_TRACE_ENABLE */

#endif
