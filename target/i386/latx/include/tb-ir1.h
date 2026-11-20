/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _TB_IR1_H_
#define _TB_IR1_H_

static inline IR1_INST *ir1_inst_in_list(struct IR1_INST *_ir1_instructions,
                                         const int i)
{
    return _ir1_instructions + i;
}

static inline int tb_ir1_num(TranslationBlock *tb)
{
    return tb->icount;
}

#endif
