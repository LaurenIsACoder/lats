/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_TR_SSE41_H_
#define _LATX_SYS_TR_SSE41_H_

bool latxs_translate_ptest(IR1_INST *pir1);
bool latxs_translate_pextrq(IR1_INST *pir1);
bool latxs_translate_pextrd(IR1_INST *pir1);
bool latxs_translate_pextrb(IR1_INST *pir1);
bool latxs_translate_pinsrb(IR1_INST *pir1);
bool latxs_translate_pinsrd(IR1_INST *pir1);
bool latxs_translate_pinsrq(IR1_INST *pir1);
bool latxs_translate_pmovzxbq(IR1_INST *pir1);
bool latxs_translate_pmovzxdq(IR1_INST *pir1);
bool latxs_translate_pmovzxbw(IR1_INST *pir1);
bool latxs_translate_pmovzxbd(IR1_INST *pir1);
bool latxs_translate_pmovzxwd(IR1_INST *pir1);
bool latxs_translate_pmovzxwq(IR1_INST *pir1);
bool latxs_translate_pmovsxbw(IR1_INST *pir1);
bool latxs_translate_pmovsxdq(IR1_INST *pir1);
bool latxs_translate_pmovsxbd(IR1_INST *pir1);
bool latxs_translate_pmovsxwd(IR1_INST *pir1);
bool latxs_translate_pmovsxbq(IR1_INST *pir1);
bool latxs_translate_pmovsxwq(IR1_INST *pir1);
bool latxs_translate_blendvpd(IR1_INST *pir1);
bool latxs_translate_blendvps(IR1_INST *pir1);
bool latxs_translate_roundss(IR1_INST *pir1);
bool latxs_translate_roundps(IR1_INST *pir1);
bool latxs_translate_roundsd(IR1_INST *pir1);
bool latxs_translate_roundpd(IR1_INST *pir1);
bool latxs_translate_pcmpeqq(IR1_INST *pir1);
bool latxs_translate_pblendw(IR1_INST *pir1);
bool latxs_translate_pblendvb(IR1_INST *pir1);
bool latxs_translate_insertps(IR1_INST *pir1);
bool latxs_translate_extractps(IR1_INST *pir1);
bool latxs_translate_pminud(IR1_INST *pir1);
bool latxs_translate_pminsd(IR1_INST *pir1);
bool latxs_translate_pminuw(IR1_INST *pir1);
bool latxs_translate_pminsb(IR1_INST *pir1);
bool latxs_translate_pmulld(IR1_INST *pir1);
bool latxs_translate_pmuldq(IR1_INST *pir1);
bool latxs_translate_pmaxsd(IR1_INST *pir1);
bool latxs_translate_pmaxud(IR1_INST *pir1);
bool latxs_translate_pmaxsb(IR1_INST *pir1);
bool latxs_translate_pmaxuw(IR1_INST *pir1);
bool latxs_translate_packusdw(IR1_INST *pir1);
bool latxs_translate_dppd(IR1_INST *pir1);
bool latxs_translate_dpps(IR1_INST *pir1);
bool latxs_translate_movntdqa(IR1_INST *pir1);
bool latxs_translate_blendpd(IR1_INST *pir1);
bool latxs_translate_blendps(IR1_INST *pir1);

#endif

