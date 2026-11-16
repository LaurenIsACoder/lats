/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_TR_MMIO_H_
#define _LATX_SYS_TR_MMIO_H_

bool latxs_translate_mov_opt_apic(IR1_INST *pir1);
bool latxs_translate_bt_opt_apic(IR1_INST *pir1);

bool latxs_translate_mov_opt_hpet(IR1_INST *pir1);

#endif
