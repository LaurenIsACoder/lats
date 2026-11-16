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
#include "sys-aot.h"

int intb_njc_enabled(void)
{
    return option_intb_njc;
}

#define LISA_LOAD_PC    LISA_LD_D
#define LISA_STORE_PC   LISA_ST_D

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
    if (option_table_addr) {
        /* Move the low hash bits next to the page bits, then pick both. */
        la_out2ii(LISA_BSTRINS_D, tmp, tmp, TARGET_PAGE_BITS - 1,
                  TARGET_PAGE_BITS - TB_JMP_PAGE_BITS);
        la_out2ii(LISA_BSTRPICK_D, hash, tmp,
                  TARGET_PAGE_BITS + TB_JMP_CACHE_BITS - TB_JMP_PAGE_BITS - 1,
                  TARGET_PAGE_BITS - TB_JMP_PAGE_BITS);
        return;
    }
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

bool latxs_tr_gen_compact_njc_slot(void *pc_ptr, void *entry_ptr,
        void *tmp_ptr, bool use_layout)
{
    IR2_OPND *pc = pc_ptr, *entry = entry_ptr, *tmp = tmp_ptr;
    unsigned bits = latx_jmp_cache_bits();
    LatxJmpCacheHashMode mode = latx_jmp_cache_hash_mode();

    if (!use_layout) {
        return false;
    }
    G_STATIC_ASSERT(sizeof(LatxNJCEntry) == 32);
    g_assert(pc->val != entry->val && pc->val != tmp->val &&
             entry->val != tmp->val);
    if (mode == LATX_JMP_CACHE_HASH_PAGE) {
        bits = TB_JMP_CACHE_BITS;
        la_out2i(LISA_SRLI_D, entry, pc, TARGET_PAGE_BITS - TB_JMP_PAGE_BITS);
        la_out3(LISA_XOR, tmp, pc, entry);
        la_out2i(LISA_LD_D, entry, &latxs_env_ir2_opnd,
                 lsenv_offset_of_latx_njc_cache_ptr(lsenv));
        la_out2ii(LISA_BSTRINS_D, entry, tmp, TB_JMP_PAGE_BITS + 4, 5);
        la_out2i(LISA_SRLI_D, tmp, tmp, TARGET_PAGE_BITS);
        la_out2ii(LISA_BSTRINS_D, entry, tmp, bits + 4, TB_JMP_PAGE_BITS + 5);
        return true;
    }
    if (mode == LATX_JMP_CACHE_HASH_FULL) {
        la_out2i(LISA_SRLI_D, entry, pc, bits);
        la_out3(LISA_XOR, tmp, pc, entry);
    } else {
        g_assert(mode == LATX_JMP_CACHE_HASH_LINE);
        tmp = pc;
    }
    /* Alignment leaves the entire slot-index field zero. BSTRINS masks,
     * scales and combines it with the base without an executed add. */
    la_out2i(LISA_LD_D, entry, &latxs_env_ir2_opnd,
             lsenv_offset_of_latx_njc_cache_ptr(lsenv));
    la_out2ii(LISA_BSTRINS_D, entry, tmp, bits + 4, 5);
    return true;
}

static void latxs_tr_gen_intb_njc_slot(IR2_OPND *gpc, bool compact,
        IR2_OPND *entry, bool use_layout)
{
    IR2_OPND *stmp1 = &latxs_stmp1_ir2_opnd; /* $a4 */
    IR2_OPND *stmp2 = &latxs_stmp2_ir2_opnd; /* $a5 */
    if (compact && latxs_tr_gen_compact_njc_slot(gpc, entry, stmp2,
                                                 use_layout)) {
        latxs_append_ir2_opnd2i(LISA_LD_D, &latxs_ret0_ir2_opnd, entry, 0);
        return;
    }
    latxs_out_jmp_cache_hash(gpc, stmp1, stmp2);

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

    if (!compact && !use_layout && hamt_enabled()) {
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
        void *njc_miss, void *slot_out, bool use_layout)
{
    IR2_OPND *ret0 = &latxs_ret0_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;

    if (intb_njc_enabled()) {
        IR2_OPND gpc = latxs_arg1_ir2_opnd;
        IR2_OPND flags = latxs_ra_alloc_itemp();
        IR2_OPND entry = slot_out ? *(IR2_OPND *)slot_out : latxs_ra_alloc_itemp();

        (void)code_ptr;
        (void)tmp;
        (void)tmp1;
        latxs_tr_gen_current_flags(&flags, tmp0);

        latxs_tr_gen_intb_njc_slot(&gpc, true, &entry, use_layout);
        latxs_append_ir2_opnd3(LISA_BEQ, ret0, zero, njc_miss);

        latxs_append_ir2_opnd2i(LISA_LOAD_PC, tmp0, &entry,
                offsetof(LatxNJCEntry, pc));
        latxs_append_ir2_opnd3(LISA_BNE, tmp0, &gpc, njc_miss);
        latxs_append_ir2_opnd2i(LISA_LD_WU, tmp0, &entry,
                offsetof(LatxNJCEntry, flags));
        latxs_append_ir2_opnd3(LISA_BNE, tmp0, &flags, njc_miss);
#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
        if (aot_tu_enabled() && hamt_enabled() && hamt_mode_dmw() &&
            option_soft_ras && option_rastoptp &&
            option_int_opt == IRQ_CHECK_ASYNC_NOTIFY) {
            /* Keep the cached ordinary address for invalidation. A native
             * destination must leave the current host frame via the miss. */
            latxs_append_ir2_opnd2i(LISA_LD_D, tmp0, &entry,
                    offsetof(LatxNJCEntry, call_tcptr));
            latxs_append_ir2_opnd3(LISA_BEQ, tmp0, zero, njc_miss);
            latxs_append_ir2_opnd3(LISA_BNE, tmp0, ret0, njc_miss);
        }
#endif
        if (!slot_out) {
            latxs_ra_free_temp(&entry);
        }
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

        latxs_tr_gen_current_flags(&flags, tmp0);

        /* Only startup stubs can assume this run's DMW base convention. */
        latxs_tr_gen_intb_njc_slot(&gpc, false, &entry,
                                   code_ptr && option_table_addr);
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

static void gen_latxs_intb_lookup_njc_compact_call(void *tmp0,
        void *njc_miss, void *slot_out, bool use_layout)
{
    IR2_OPND *ret0 = &latxs_ret0_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;
    IR2_OPND *stmp1 = &latxs_stmp1_ir2_opnd;
    IR2_OPND *stmp2 = &latxs_stmp2_ir2_opnd;

    if (intb_njc_enabled()) {
        IR2_OPND gpc = latxs_arg1_ir2_opnd;
        IR2_OPND flags = latxs_ra_alloc_itemp();
        IR2_OPND entry = slot_out ? *(IR2_OPND *)slot_out : latxs_ra_alloc_itemp();

        latxs_tr_gen_current_flags(&flags, tmp0);
        if (!latxs_tr_gen_compact_njc_slot(&gpc, &entry, stmp2, use_layout)) {
            latxs_out_jmp_cache_hash(&gpc, stmp1, stmp2);
            latxs_append_ir2_opnd2i(LISA_LD_D, stmp1, &latxs_env_ir2_opnd,
                    lsenv_offset_of_latx_njc_cache_ptr(lsenv));
            latxs_append_ir2_opnd2i(LISA_SLLI_D, stmp2, stmp2, 5);
            latxs_append_ir2_opnd3(LISA_ADD_D, stmp1, stmp2, stmp1);
            latxs_append_ir2_opnd2_(lisa_mov, &entry, stmp1);
        }
        latxs_append_ir2_opnd2i(LISA_LD_D, ret0, &entry,
                offsetof(LatxNJCEntry, call_tcptr));
        latxs_append_ir2_opnd3(LISA_BEQ, ret0, zero, njc_miss);
        latxs_append_ir2_opnd2i(LISA_LOAD_PC, tmp0, &entry,
                offsetof(LatxNJCEntry, pc));
        latxs_append_ir2_opnd3(LISA_BNE, tmp0, &gpc, njc_miss);
        latxs_append_ir2_opnd2i(LISA_LD_WU, tmp0, &entry,
                offsetof(LatxNJCEntry, flags));
        latxs_append_ir2_opnd3(LISA_BNE, tmp0, &flags, njc_miss);
        if (!slot_out) {
            latxs_ra_free_temp(&entry);
        }
        latxs_ra_free_temp(&flags);
    }
}

static void gen_latxs_intb_lookup_njc_inline_impl(bool compact,
        IR2_OPND *cold_miss, IR2_OPND *call_label, IR2_OPND *jump_target,
        bool use_layout, bool call_target)
{
    IR2_OPND *ret0 = &latxs_ret0_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;
    IR2_OPND tmp = latxs_ra_alloc_itemp();
    IR2_OPND tmp0 = latxs_ra_alloc_itemp();
    IR2_OPND tmp1 = latxs_ra_alloc_itemp();
    IR2_OPND njc_miss = cold_miss ? *cold_miss : latxs_ir2_opnd_new_label();
    int lookup_start = lsenv->tr_data->ir2_asm_nr;

    if (compact) {
        if (call_target) {
            gen_latxs_intb_lookup_njc_compact_call(&tmp0, &njc_miss, &la_ra,
                                                   use_layout);
        } else {
            gen_latxs_intb_lookup_njc_compact(NULL,
                    &tmp, &tmp0, &tmp1, &njc_miss, &la_ra, use_layout);
        }
    } else {
        gen_latxs_intb_lookup_njc_legacy(NULL,
                &tmp, &tmp0, &tmp1, &njc_miss, &la_ra);
        if (hamt_enabled()) {
            latxs_tr_dmw_in_fix_mem(&tmp);
        }
    }
    if (cold_miss) {
        if (compact && option_aot_save && !in_aot_translate) {
            /* Only actual guest CALLs produce entry evidence. */
            latxs_append_ir2_opnd2i(LISA_LD_HU, &tmp0, &la_ra,
                    offsetof(LatxNJCEntry, observed_call_entry));
            latxs_append_ir2_opnd3(LISA_BEQ, &tmp0, zero, cold_miss);
        }
        /* Its near miss island immediately precedes this lookup. */
        g_assert(lsenv->tr_data->ir2_asm_nr - lookup_start < 32767);
        *jump_target = compact ? *ret0 : tmp;
        latxs_append_ir2_opnd1(LISA_LABEL, call_label);
    }
    latxs_append_ir2_opnd2i(LISA_JIRL,
            cold_miss && option_tu_hras == 2 ? &la_ra : zero,
            compact ? ret0 : &tmp, 0);
    if (!cold_miss) {
        latxs_append_ir2_opnd1(LISA_LABEL, &njc_miss);
        latxs_tr_gen_intb_sync_gpc();
    }

    latxs_ra_free_temp(&tmp1);
    latxs_ra_free_temp(&tmp0);
    latxs_ra_free_temp(&tmp);
}

void gen_latxs_intb_lookup_njc_inline(bool compact, bool use_layout)
{
    gen_latxs_intb_lookup_njc_inline_impl(compact, NULL, NULL, NULL,
                                          use_layout, false);
}

void gen_latxs_intb_lookup_njc_inline_target(bool compact, bool use_layout,
        bool call_target)
{
    gen_latxs_intb_lookup_njc_inline_impl(compact, NULL, NULL, NULL,
                                          use_layout, compact && call_target);
}

int gen_latxs_intb_njc_return(void *code_ptr)
{
    int count;

    latxs_tr_init(NULL);
    IR2_OPND tmp = latxs_ra_alloc_itemp();
    IR2_OPND tmp0 = latxs_ra_alloc_itemp();
    IR2_OPND tmp1 = latxs_ra_alloc_itemp();
    IR2_OPND miss = la_new_label();

    /* This is a leaf lookup: no helper calls and no cached-slot result in
     * ra. The allocator excludes ra, tp, sp, a1 and the previous-TB a6.
     * Both outcomes return through the same live caller link. */
    gen_latxs_intb_lookup_njc_compact(code_ptr, &tmp, &tmp0, &tmp1,
                                    &miss, NULL, option_table_addr);
    la_out2i(LISA_JIRL, &la_zero, &la_ra, 0);
    la_label(miss);
    /* Skip the caller's single target JIRL on a miss. Hits retain the
     * hardware-predicted return address; only the cold miss changes
     * the return target. Both paths consume exactly one return. */
    la_out2i(LISA_ADDI_D, &la_ra, &la_ra, 4);
    la_out2i(LISA_JIRL, &la_zero, &la_ra, 0);
    latxs_ra_free_temp(&tmp1);
    latxs_ra_free_temp(&tmp0);
    latxs_ra_free_temp(&tmp);
    count = latxs_tr_ir2_assemble(code_ptr);
    latxs_tr_fini();
    return count;
}

void gen_latxs_intb_lookup_njc_call(bool compact, void *miss_label,
        void *call_label, void *jump_target)
{
    gen_latxs_intb_lookup_njc_inline_impl(compact,
            miss_label, call_label, jump_target, option_njc_layout, compact);
}
