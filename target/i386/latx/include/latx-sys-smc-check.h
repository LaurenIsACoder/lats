/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_SMC_CHECK_H_
#define _LATX_SYS_SMC_CHECK_H_

//#define CONFIG_LATX_SMC_INST_SELF_CHECK

#ifdef CONFIG_LATX_SMC_INST_SELF_CHECK
void latxs_tr_gen_smc_inst_self_check(TranslationBlock *tb, 
                                      IR1_INST *pir1);
#else
static inline
void latxs_tr_gen_smc_inst_self_check(TranslationBlock *tb,
                                      IR1_INST *pir1) { }
#endif

#endif
