/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_H_
#define _HAMT_H_

#include "hamt-dmw.h"

/* Reserved instruction at an invalidated translated-code entry. */
#define HAMT_INVALID_TB_INSN 0x1
void hamt_poison_tb_entry(TranslationBlock *tb);

void hamt_tlb_handler(uint64_t hamt_badvaddr,
        CPUX86State *env, uint32_t *epc);
void hamt_fpe_handler(CPUX86State *env, uint32_t *epc, uint32_t fcsr);
void hamt_err_handler(uint64_t hamt_badvaddr,
        CPUX86State *env, uint32_t *epc);
void hamt_ale_handler(uint64_t hamt_badvaddr,
        CPUX86State *env, uint32_t *epc);
void hamt_ine_handler(uint64_t estat, CPUX86State *env, uint32_t *epc);

void hamt_ras_tp_mirror(int cpuid, uint64_t native_tp);
void hamt_native_resume_iret(CPUX86State *env);
void hamt_native_fault_clear(CPUX86State *env);
TranslationBlock *hamt_native_resume_tb(CPUX86State *env);
void hamt_native_resume_entry(void);
void *hamt_native_async_frame(CPUX86State *env, uintptr_t pc, uintptr_t sp);
void hamt_native_async_entry(void);

#endif
