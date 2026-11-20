/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_TB_EXEC_TRACE_H_
#define _LATX_TB_EXEC_TRACE_H_

#if defined(CONFIG_LATX_TB_EXEC_TRACE)

#define LATX_TB_EXEC_TRACE_MAX      (1ULL << 10)
#define LATX_TB_EXEC_TRACE_IDMASK   ((1ULL << 10) - 1)

void *latx_new_tb_exec_trace(void);
void latx_print_tb_exec_trace(void **data, uint64_t idx);
void latx_tr_gen_tb_exec_trace(void *tb);

#endif

#endif
