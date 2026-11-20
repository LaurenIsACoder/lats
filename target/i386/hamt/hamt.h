/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_H_
#define _HAMT_H_

#include "hamt-dmw.h"

void hamt_tlb_handler(uint64_t hamt_badvaddr,
        CPUX86State *env, uint32_t *epc);
void hamt_fpe_handler(CPUX86State *env, uint32_t *epc, uint32_t fcsr);
void hamt_err_handler(uint64_t hamt_badvaddr,
        CPUX86State *env, uint32_t *epc);
void hamt_ale_handler(uint64_t hamt_badvaddr,
        CPUX86State *env, uint32_t *epc);

#endif
