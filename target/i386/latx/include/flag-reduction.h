/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _FLAG_REDUCTION_H_
#define _FLAG_REDUCTION_H_

#include "latx-types.h"

typedef struct {
    uint8 use;
    uint8 def;
    uint8 undef;
} IR1_EFLAG_USEDEF;

#define __INVALID    (1 << 7)
#define __VALID      (0)

#define __CF         ((1 << CF_USEDEF_BIT_INDEX) | __VALID)
#define __PF         ((1 << PF_USEDEF_BIT_INDEX) | __VALID)
#define __AF         ((1 << AF_USEDEF_BIT_INDEX) | __VALID)
#define __ZF         ((1 << ZF_USEDEF_BIT_INDEX) | __VALID)
#define __SF         ((1 << SF_USEDEF_BIT_INDEX) | __VALID)
#define __OF         ((1 << OF_USEDEF_BIT_INDEX) | __VALID)
#define __DF         ((1 << DF_USEDEF_BIT_INDEX) | __VALID)

#define __NONE       (0)
#define __OSAPF      (__OF | __SF | __AF | __PF)
#define __OSZPF      (__OF | __SF | __ZF | __PF)
#define __SZAPF      (__SF | __ZF | __AF | __PF)
#define __SZAPCF     (__SZAPF | __CF)
#define __ALL_EFLAGS (__OF | __SF | __ZF | __AF | __PF | __CF)

void tb_flag(struct TranslationBlock *tb);
void __latxs_flag_reduction_ex(uint8_t *last_pending, uint8_t *first_def, TranslationBlock *tb);
void __latxs_flag_reduction_ex1(uint8_t *last_pending, uint8_t *first_def, TranslationBlock *tb);
void __latxs_flag_unreduction_ex(TranslationBlock *tb);
void latxs_flag_reduction_parse_use(TranslationBlock *tb);

#endif
