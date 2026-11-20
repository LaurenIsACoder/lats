/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _TCG_SPLIT_TB_H_
#define _TCG_SPLIT_TB_H_

#define SPLIT_TB_BASE   0x9b00000000ULL

extern int split_tb_base_stat;
static inline int split_tb_base_ok(void) { return split_tb_base_stat; }

#endif
