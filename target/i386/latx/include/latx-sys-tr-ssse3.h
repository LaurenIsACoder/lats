/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_TR_SSSE3_H_
#define _LATX_SYS_TR_SSSE3_H_

bool latxs_translate_palignr(IR1_INST *pir1);
bool latxs_translate_pshufb(IR1_INST *pir1);
bool latxs_translate_psignb(IR1_INST *pir1);
bool latxs_translate_psignw(IR1_INST *pir1);
bool latxs_translate_psignd(IR1_INST *pir1);
bool latxs_translate_haddpd(IR1_INST *pir1);
bool latxs_translate_haddps(IR1_INST *pir1);
bool latxs_translate_hsubpd(IR1_INST *pir1);
bool latxs_translate_hsubps(IR1_INST *pir1);
bool latxs_translate_phaddd(IR1_INST *pir1);
bool latxs_translate_phaddw(IR1_INST *pir1);
bool latxs_translate_phaddsw(IR1_INST *pir1);
bool latxs_translate_phsubd(IR1_INST *pir1);
bool latxs_translate_phsubw(IR1_INST *pir1);
bool latxs_translate_phsubsw(IR1_INST *pir1);
bool latxs_translate_pabsb(IR1_INST *pir1);
bool latxs_translate_pabsw(IR1_INST *pir1);
bool latxs_translate_pabsd(IR1_INST *pir1);
bool latxs_translate_movshdup(IR1_INST *pir1);
bool latxs_translate_movsldup(IR1_INST *pir1);
bool latxs_translate_movddup(IR1_INST *pir1);
bool latxs_translate_pmaddubsw(IR1_INST *pir1);
bool latxs_translate_pmulhrsw(IR1_INST *pir1);

#endif
