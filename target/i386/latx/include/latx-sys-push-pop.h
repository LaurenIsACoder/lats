/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_PUSH_POP_H_
#define _LATX_SYS_PUSH_POP_H_
#include "common.h"
#include "ir1.h"
#include "lsenv.h"
#include "latx-options.h"

bool latx_adjust_sp_before_translate(TranslationBlock *tb, IR1_INST *pir1, int index);
void latx_restore_sp(void);
#endif