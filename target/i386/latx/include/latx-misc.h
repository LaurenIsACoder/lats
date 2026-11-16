/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef LATX_MISC_H
#define LATX_MISC_H
#include "loongarch-extcontext.h"
#include "latx/include/ir1.h"
#include "latx/include/ir2.h"
#include "lats/aot-format.h"

/* mask */
#define MF_RAS_CPU_MASK        0xf0000000
#define MF_RAS_CPU_SHIFT       28
#define MF_MTCC_ID_MASK        0x0f000000
#define MF_MTCC_ID_SHIFT       24
#define MF_LINK0_MASK          0x00f00000
#define MF_LINK0_SHIFT         20
#define MF_LINK1_MASK          0x000f0000
#define MF_LINK1_SHIFT         16

/* bit flags */
#define MF_FLAGS_NOEFLAGS      0x00008000
#define MF_END_SYSCALL         0x00004000
#define MF_NTB_CROSSPAGE_0     0x00002000
#define MF_NTB_CROSSPAGE_1     0x00001000
#define MF_RAS_LINKED          0x00000800
#define MF_RAS_CLEAR_B0        0x00000400
#define MF_MMIO_OPT            0x00000200
#define MF_OBSERVED_CALL_ENTRY AOT_BLOCK_OBSERVED_CALL_ENTRY
/* Be careful not to define other flags on the lower 8bit */
#define MF_LINK_STATUS         0x00000001

bool check_misc_flags(TranslationBlock *tb, uint32_t flags);
void set_misc_flags(TranslationBlock *tb, uint32_t flags);
void clear_misc_flags(TranslationBlock *tb);
void add_misc_flags(TranslationBlock *tb, uint32_t flags);
void remove_misc_flags(TranslationBlock *tb, uint32_t flags);
uint32_t get_mask_from_miscflags(const TranslationBlock *tb, uint32_t m,
                                 int s);
void set_mask_to_miscflags(TranslationBlock *tb, uint32_t value,
                           uint32_t mask, int shift);

#endif
