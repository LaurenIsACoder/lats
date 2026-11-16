/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <string.h>
#include "common.h"
#include "cpu.h"
#include "lsenv.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "sys-excp.h"
#include "latx-misc.h"
#include "latx-native-asm.h"
#include "loongarch-extcontext.h"

bool check_misc_flags(TranslationBlock *tb, uint32_t flags)
{
    return tb->misc_flags & flags;
}

void set_misc_flags(TranslationBlock *tb, uint32_t flags)
{
    tb->misc_flags = flags;
}

void clear_misc_flags(TranslationBlock *tb)
{
    tb->misc_flags = 0;
}

void add_misc_flags(TranslationBlock *tb, uint32_t flags)
{
    tb->misc_flags |= flags;
}

void remove_misc_flags(TranslationBlock *tb, uint32_t flags)
{
    tb->misc_flags &= ~flags;
}

uint32_t get_mask_from_miscflags(const TranslationBlock *tb, uint32_t m,
                                 int s)
{
    uint32_t ret;

    ret = (tb->misc_flags & m) >> s;
    return ret;
}

void set_mask_to_miscflags(TranslationBlock *tb, uint32_t value,
                           uint32_t mask, int shift)
{
    remove_misc_flags(tb, mask);
    uint32_t flags = value << shift;
    add_misc_flags(tb, flags);
}
