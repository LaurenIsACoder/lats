/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <string.h>
#include "common.h"
#include "cpu.h"
#include "lsenv.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "sys-excp.h"
#include "latx-static-codes.h"
#include "latx-intb-sys.h"
#include "latx-sys-hamt-dmw.h"
#include "qemu/cacheflush.h"
#include "accel/tcg/tb-hash.h"

int intb_njc_enabled(void)
{
    return option_intb_njc;
}

#define LISA_LOAD_PC    LISA_LD_D
#define LISA_STORE_PC   LISA_ST_D

void latxs_tr_gen_jmp_cache_hash(void *pc_ptr, void *tmp_ptr, void *hash_ptr)
{
    IR2_OPND *pc = pc_ptr;
    IR2_OPND *tmp = tmp_ptr;
    IR2_OPND *hash = hash_ptr;

    switch (latx_jmp_cache_hash_mode()) {
    case LATX_JMP_CACHE_HASH_LINE:
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, hash, pc,
                latx_jmp_cache_bits() - 1, 0);
        return;
    case LATX_JMP_CACHE_HASH_FULL:
        latxs_append_ir2_opnd2i(LISA_SRLI_D, tmp, pc,
                latx_jmp_cache_bits());
        latxs_append_ir2_opnd3(LISA_XOR, tmp, pc, tmp);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, hash, tmp,
                latx_jmp_cache_bits() - 1, 0);
        return;
    case LATX_JMP_CACHE_HASH_PAGE:
        break;
    default:
        g_assert_not_reached();
    }

#ifdef CONFIG_SOFTMMU
    latxs_append_ir2_opnd2i(LISA_SRLI_D, tmp, pc,
            TARGET_PAGE_BITS - TB_JMP_PAGE_BITS);
    latxs_append_ir2_opnd3(LISA_XOR, tmp, pc, tmp);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, hash, tmp,
            TARGET_PAGE_BITS + TB_JMP_CACHE_BITS - TB_JMP_PAGE_BITS - 1,
            TARGET_PAGE_BITS);
    latxs_append_ir2_opnd2i(LISA_SLLI_D, hash, hash, TB_JMP_PAGE_BITS);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, hash, tmp,
            TB_JMP_PAGE_BITS - 1, 0);
#else
    latxs_append_ir2_opnd2i(LISA_SRLI_D, tmp, pc, TB_JMP_CACHE_BITS);
    latxs_append_ir2_opnd3(LISA_XOR, tmp, pc, tmp);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, hash, tmp,
            TB_JMP_CACHE_BITS - 1, 0);
#endif
}

void latxs_out_jmp_cache_hash(void *pc_ptr, void *tmp_ptr, void *hash_ptr)
{
    IR2_OPND *pc = pc_ptr;
    IR2_OPND *tmp = tmp_ptr;
    IR2_OPND *hash = hash_ptr;

    switch (latx_jmp_cache_hash_mode()) {
    case LATX_JMP_CACHE_HASH_LINE:
        la_out2ii(LISA_BSTRPICK_D, hash, pc,
                  latx_jmp_cache_bits() - 1, 0);
        return;
    case LATX_JMP_CACHE_HASH_FULL:
        la_out2i(LISA_SRLI_D, tmp, pc, latx_jmp_cache_bits());
        la_out3(LISA_XOR, tmp, pc, tmp);
        la_out2ii(LISA_BSTRPICK_D, hash, tmp,
                  latx_jmp_cache_bits() - 1, 0);
        return;
    case LATX_JMP_CACHE_HASH_PAGE:
        break;
    default:
        g_assert_not_reached();
    }

#ifdef CONFIG_SOFTMMU
    la_out2i(LISA_SRLI_D, tmp, pc,
             TARGET_PAGE_BITS - TB_JMP_PAGE_BITS);
    la_out3(LISA_XOR, tmp, pc, tmp);
    la_out2ii(LISA_BSTRPICK_D, hash, tmp,
              TARGET_PAGE_BITS + TB_JMP_CACHE_BITS - TB_JMP_PAGE_BITS - 1,
              TARGET_PAGE_BITS);
    la_out2i(LISA_SLLI_D, hash, hash, TB_JMP_PAGE_BITS);
    la_out2ii(LISA_BSTRINS_D, hash, tmp, TB_JMP_PAGE_BITS - 1, 0);
#else
    la_out2i(LISA_SRLI_D, tmp, pc, TB_JMP_CACHE_BITS);
    la_out3(LISA_XOR, tmp, pc, tmp);
    la_out2ii(LISA_BSTRPICK_D, hash, tmp,
              TB_JMP_CACHE_BITS - 1, 0);
#endif
}

static void latxs_tr_gen_intb_njc_slot(IR2_OPND *gpc, bool compact,
        IR2_OPND *entry)
{
    IR2_OPND *stmp1 = &latxs_stmp1_ir2_opnd; /* $a4 */
    IR2_OPND *stmp2 = &latxs_stmp2_ir2_opnd; /* $a5 */
    latxs_tr_gen_jmp_cache_hash(gpc, stmp1, stmp2);

    latxs_append_ir2_opnd2i(LISA_LD_D, stmp1, &latxs_env_ir2_opnd,
            compact ? lsenv_offset_of_latx_njc_cache_ptr(lsenv) :
                      lsenv_offset_of_tb_jmp_cache_ptr(lsenv));

    if (compact) {
        /* sizeof(LatxNJCEntry) == 32; ALSL.D cannot scale by 32. */
        latxs_append_ir2_opnd2i(LISA_SLLI_D, stmp2, stmp2, 5);
        latxs_append_ir2_opnd3(LISA_ADD_D, stmp1, stmp2, stmp1);
    } else {
#ifdef LATX_PCREL
        latxs_append_ir2_opnd3i(LISA_ALSL_D, stmp1, stmp2, stmp1, 3);
#else
        latxs_append_ir2_opnd3i(LISA_ALSL_D, stmp1, stmp2, stmp1, 2);
#endif
    }

    if (!compact && hamt_enabled()) {
        latxs_tr_dmw_in_fix_mem(stmp1);
    }
    latxs_append_ir2_opnd2_(lisa_mov, entry, stmp1);
    latxs_append_ir2_opnd2i(LISA_LD_D, &latxs_ret0_ir2_opnd, stmp1, 0);
}

static void latxs_tr_gen_current_flags(IR2_OPND *flags, IR2_OPND *tmp)
{
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
    latxs_append_ir2_opnd2i(LISA_LD_WU, flags, &latxs_env_ir2_opnd,
            offsetof(CPUX86State, hflags));
#else
    IR2_OPND eflags = latxs_ra_alloc_itemp();

    latxs_append_ir2_opnd2i(LISA_LD_WU, &eflags, &latxs_env_ir2_opnd,
            lsenv_offset_of_eflags(lsenv));
    latxs_load_imm64_to_ir2(tmp,
            IOPL_MASK | TF_MASK | RF_MASK | VM_MASK | AC_MASK);
    latxs_append_ir2_opnd3(LISA_AND, tmp, &eflags, tmp);
    latxs_ra_free_temp(&eflags);
    latxs_append_ir2_opnd2i(LISA_LD_WU, flags, &latxs_env_ir2_opnd,
            offsetof(CPUX86State, hflags));
    latxs_append_ir2_opnd3(LISA_OR, flags, flags, tmp);
#endif
}

void gen_latxs_intb_lookup_njc_compact(void *code_ptr,
        void *tmp, void *tmp0, void *tmp1,
        void *njc_miss, void *slot_out)
{
    IR2_OPND *ret0 = &latxs_ret0_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;

    if (intb_njc_enabled()) {
        IR2_OPND gpc = latxs_arg1_ir2_opnd;
        IR2_OPND flags = latxs_ra_alloc_itemp();
        IR2_OPND entry = latxs_ra_alloc_itemp();

        (void)code_ptr;
        (void)tmp;
        (void)tmp1;
        latxs_tr_gen_current_flags(&flags, tmp0);

        latxs_tr_gen_intb_njc_slot(&gpc, true, &entry);
        if (slot_out) {
            latxs_append_ir2_opnd2_(lisa_mov, slot_out, &entry);
        }
        latxs_append_ir2_opnd3(LISA_BEQ, ret0, zero, njc_miss);

        latxs_append_ir2_opnd2i(LISA_LOAD_PC, tmp0, &entry,
                offsetof(LatxNJCEntry, pc));
        latxs_append_ir2_opnd3(LISA_BNE, tmp0, &gpc, njc_miss);
        latxs_append_ir2_opnd2i(LISA_LD_WU, tmp0, &entry,
                offsetof(LatxNJCEntry, flags));
        latxs_append_ir2_opnd3(LISA_BNE, tmp0, &flags, njc_miss);
        latxs_ra_free_temp(&entry);
        latxs_ra_free_temp(&flags);
    }
}

void gen_latxs_intb_lookup_njc_legacy(void *code_ptr,
        void *tmp, void *tmp0, void *tmp1,
        void *njc_miss, void *slot_out)
{
    IR2_OPND *ret0 = &latxs_ret0_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;

    if (intb_njc_enabled()) {
        IR2_OPND gpc = latxs_arg1_ir2_opnd;
        IR2_OPND flags = latxs_ra_alloc_itemp();
        IR2_OPND entry = latxs_ra_alloc_itemp();

        (void)code_ptr;
        latxs_tr_gen_current_flags(&flags, tmp0);

        latxs_tr_gen_intb_njc_slot(&gpc, false, &entry);
        if (slot_out) {
            latxs_append_ir2_opnd2_(lisa_mov, slot_out, &entry);
        }
        latxs_append_ir2_opnd3(LISA_BEQ, ret0, zero, njc_miss);
        if (hamt_enabled()) {
            latxs_tr_dmw_in_fix_mem(ret0);
        }
        latxs_append_ir2_opnd2i(LISA_LOAD_PC, tmp0, ret0,
                offsetof(TranslationBlock, pc));
        latxs_append_ir2_opnd3(LISA_BNE, tmp0, &gpc, njc_miss);

#ifdef CONFIG_SOFTMMU
        /* 1.3 check next TB cflags CF_INVALID 0x00040000 */
        latxs_append_ir2_opnd2i(LISA_LD_WU, tmp0, ret0,
                offsetof(TranslationBlock, cflags));
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, tmp0, tmp0, 18, 18);
        latxs_append_ir2_opnd3(LISA_BNE, tmp0, zero, njc_miss);
#endif

#ifdef CONFIG_SOFTMMU
        /* 1.4 check next TB's CSBASE */
        latxs_append_ir2_opnd2i(LISA_LD_WU, tmp0, ret0,
                offsetof(TranslationBlock, cs_base));
        latxs_append_ir2_opnd2i(LISA_LOAD_PC, tmp1, &latxs_env_ir2_opnd,
                offsetof(CPUX86State, segs[R_CS].base));
        latxs_append_ir2_opnd3(LISA_BNE, tmp0, tmp1, njc_miss);
#endif

#ifdef CONFIG_SOFTMMU
        /* tb->flags */
        latxs_append_ir2_opnd2i(LISA_LD_WU, tmp0, ret0,
                offsetof(TranslationBlock, flags));
        latxs_append_ir2_opnd3(LISA_BNE, tmp0, &flags, njc_miss);
#endif
        latxs_append_ir2_opnd2i(LISA_LD_D, tmp, ret0,
                offsetof(TranslationBlock, tc.ptr));
        latxs_ra_free_temp(&entry);
        latxs_ra_free_temp(&flags);
    }
}

/* a1 carries the indirect target until lookup falls back to env-based code. */
void latxs_tr_gen_intb_sync_gpc(void)
{
    latxs_tr_gen_store_to_curr_eip(&latxs_arg1_ir2_opnd);
}

void gen_latxs_intb_lookup_njc_inline(bool compact)
{
    IR2_OPND *ret0 = &latxs_ret0_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;
    IR2_OPND tmp = latxs_ra_alloc_itemp();
    IR2_OPND tmp0 = latxs_ra_alloc_itemp();
    IR2_OPND tmp1 = latxs_ra_alloc_itemp();
    IR2_OPND njc_miss = latxs_ir2_opnd_new_label();

    if (compact) {
        gen_latxs_intb_lookup_njc_compact(NULL,
                &tmp, &tmp0, &tmp1, &njc_miss, &la_ra);
    } else {
        gen_latxs_intb_lookup_njc_legacy(NULL,
                &tmp, &tmp0, &tmp1, &njc_miss, &la_ra);
        if (hamt_enabled()) {
            latxs_tr_dmw_in_fix_mem(&tmp);
        }
    }
    latxs_append_ir2_opnd2i(LISA_JIRL, zero,
            compact ? ret0 : &tmp, 0);
    latxs_append_ir2_opnd1(LISA_LABEL, &njc_miss);
    latxs_tr_gen_intb_sync_gpc();

    latxs_ra_free_temp(&tmp1);
    latxs_ra_free_temp(&tmp0);
    latxs_ra_free_temp(&tmp);
}
