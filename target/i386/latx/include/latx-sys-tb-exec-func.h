/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_TB_EXEC_FUNC_H_
#define _LATX_SYS_TB_EXEC_FUNC_H_

void latxs_before_exec_tb(CPUState *cpu, TranslationBlock *tb);
void latxs_after_exec_tb(CPUState *cpu, TranslationBlock *tb);

#endif
