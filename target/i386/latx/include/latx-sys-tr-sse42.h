/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_TR_SSE42_H_
#define _LATX_SYS_TR_SSE42_H_

bool latxs_translate_pcmpgtq(IR1_INST *pir1);
bool latxs_translate_pcmpistri(IR1_INST *pir1);
bool latxs_translate_pcmpestri(IR1_INST *pir1);
bool latxs_translate_pcmpistrm(IR1_INST *pir1);
bool latxs_translate_pcmpestrm(IR1_INST *pir1);

#endif

