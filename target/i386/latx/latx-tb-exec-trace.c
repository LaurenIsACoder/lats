/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "cpu.h"
#include "lsenv.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "latx-static-codes.h"
#include "latx-intb-sys.h"
#include "latx-misc.h"
#include "latx-cp-counter.h"
#include "latx-shadow-htable.h"
#include "latx-sys-hamt-dmw.h"
#include "latx-tb-exec-trace.h"
#include "tcg/tcg-split-tb.h"

#if defined(CONFIG_LATX_TB_EXEC_TRACE)

void *latx_new_tb_exec_trace(void)
{
    void *buffer = mmap(NULL, sizeof(void *) * LATX_TB_EXEC_TRACE_MAX,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return buffer;
}

void latx_print_tb_exec_trace(void **data, uint64_t idx)
{
    int count = 0;
    TranslationBlock *tb = NULL;
    while (count < LATX_TB_EXEC_TRACE_MAX) {
        idx = idx & LATX_TB_EXEC_TRACE_IDMASK;
        tb = data[idx];
        if (tb) {
            printf("tb exec trace [%4d] ", idx);
            printf("tb %p flags %x cflags %x ",
                    tb, tb->flags, tb->cflags);
            printf("pc %lx nextpc %lx %lx",
                    tb->pc, tb->next_pc[0], tb->next_pc[1]);
            printf("\n");
        }
        count += 1;
        idx -= 1;
    }
}

void latx_tr_gen_tb_exec_trace(void *tb)
{
    IR2_OPND tmp0 = latxs_ra_alloc_itemp();
    IR2_OPND tmp1 = latxs_ra_alloc_itemp();
    la_out2i(LISA_LD_D, &tmp0, &la_env, offsetof(CPUX86State, tb_exec_trace));
    la_out2i(LISA_LD_D, &tmp1, &la_env, offsetof(CPUX86State, tb_exec_trace_nr));
    la_out2i(LISA_ADDI_D, &tmp1, &tmp1, 1);
    la_out2i(LISA_ANDI, &tmp1, &tmp1, 0x3ff);
    la_out2i(LISA_ST_D, &tmp1, &la_env, offsetof(CPUX86State, tb_exec_trace_nr));
    la_out3i(LISA_ALSL_D, &tmp0, &tmp1, &tmp0, 2);
    la_imm(&tmp1, (uint64_t)tb);
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&tmp0);
    la_out2i(LISA_ST_D, &tmp1, &tmp0, 0);
    la_tmp_free(tmp0);
    la_tmp_free(tmp1);
}

#endif
