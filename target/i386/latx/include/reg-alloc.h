/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _REG_ALLOC_H_
#define _REG_ALLOC_H_

#include "ir2.h"

void latxs_ra_init(void);

/* allocate x86 register */
IR2_OPND latxs_ra_alloc_gpr(int);
IR2_OPND latxs_ra_alloc_vreg(int);
IR2_OPND latxs_ra_alloc_mmx(int);
IR2_OPND latxs_ra_alloc_st(int);
IR2_OPND latxs_ra_alloc_xmm(int num);

/* allocate dbt register */
IR2_OPND latxs_ra_alloc_dbt_arg1(void);

/* allocate temp register */
IR2_OPND latxs_ra_alloc_itemp(void);
IR2_OPND latxs_ra_alloc_ftemp(void);

IR2_OPND latxs_ra_alloc_ptn_itemp(void);

/* free temp register */
void latxs_ra_free_temp(IR2_OPND *);

/* temp register status */
bool latxs_ra_temp_is_free(int latxs_temp_index);

/* imm cache control temp registers */
IR2_OPND latxs_ra_alloc_imm_reg(int temp_index);

#endif
