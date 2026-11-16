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
#include "latx-perfmap.h"
#include "latx-intb-sys.h"
#include "latx-helper.h"
#include "latx-static-codes.h"
#include "latx-shadow-htable.h"
#include "latx-sys-hamt-dmw.h"
#include "latx-intb-counter.h"
#include "latx-hack.h"
#include "accel/tcg/tb-hash.h"
#include "sys-aot.h"

#define LISA_LOAD_PC    LISA_LD_D
#define LISA_STORE_PC   LISA_ST_D

#define HAMT_DMW_FIXIN(t) do {          \
    if (hamt_enabled()) {               \
        latxs_tr_dmw_in_fix_mem(&(t));  \
    }                                   \
} while (0)

#define HAMT_DMW_FIXOUT(t) do {          \
    if (hamt_enabled()) {               \
        latxs_tr_dmw_out_fix_mem(&(t));  \
    }                                   \
} while (0)

//#define FSHT_LOOKUP_CHECK_FLAGS

/* Shared lookup stubs are regenerated at startup; inline AOT code is not. */
static void latxs_out_fsht_slot(IR2_OPND *base, IR2_OPND *hash,
                                uint64_t fshtbase, bool shared_stub)
{
    if (option_table_addr && shared_stub && !(fshtbase & (FSHT_SIZE - 1))) {
        la_out2ii(LISA_BSTRINS_D, base, hash, FSHT_BITS + 2, 3);
    } else {
        la_out2ii(LISA_BSTRPICK_D, hash, hash, FSHT_BITS - 1, 0);
        la_out3i(LISA_ALSL_D, base, hash, base, 2);
    }
}

typedef enum FSHTCallPart {
    FSHT_CALL_FULL,
    FSHT_CALL_NJC,
    FSHT_CALL_MISS,
} FSHTCallPart;

static void gen_latxs_intb_fsht_lookup_legacy_body(void *code_ptr, int fid,
        bool tucpfast_version, IR2_OPND t0, IR2_OPND t1, IR2_OPND t2,
        IR2_OPND t3, IR2_OPND t4, IR2_OPND njc_miss, IR2_OPND fsht_miss,
        IR2_OPND *call_label, IR2_OPND *cold_miss,
        FSHTCallPart part)
{
    flaged_shadow_hash_table_t *f = &fsht[fid];
    uint64_t fshtbase = f->base;
    uint32_t __attribute__((unused)) fshtflag = f->flags;
    TRANSLATION_DATA *td = lsenv->tr_data;
    uint32_t mtcc_id = td->mtcc_id;
    IR2_OPND cached = call_label ? la_a0 : t1;
    IR2_OPND result = call_label ? la_a0 : t2;
#ifdef CONFIG_SOFTMMU
    int mmuidx = MMU_KNOSMAP_IDX;
    if ((fshtflag & 0x3) == 3) {
        mmuidx = MMU_USER_IDX;
    }
#endif

    if (part != FSHT_CALL_MISS) {
    // 0.2 calculate hash for jmp cache
    // a1 = GPC
    if (intb_njc_enabled() && (!intb_njc_inline_enabled() || call_label)) {
    latxs_out_jmp_cache_hash(&la_a1, &t1, &t2);

    // 0.3 load TB from jmp cache
    // a1 = GPC
    // t2 = index for jmp cache
    la_out2i(LISA_LD_D, &t1, &la_env,
            lsenv_offset_of_tb_jmp_cache_ptr(lsenv));
#ifdef LATX_PCREL
    la_out3i(LISA_ALSL_D, &t1, &t2, &t1, 3);
#else
    la_out3i(LISA_ALSL_D, &t1, &t2, &t1, 2);
#endif

    if (!code_ptr || !option_table_addr) {
        HAMT_DMW_FIXIN(t1);
    }
    if (!call_label || part == FSHT_CALL_NJC) {
        la_out2_(lisa_mov, &la_ra, &t1);
    }
    la_out2i(LISA_LD_D, &cached, &t1, 0);
    /* The NJC is per-CPU; its lookup does not need an acquire barrier. */
    la_comment("load TB from jmp cache");
    }

    la_out2i(LISA_LD_WU, &t4, &la_env, offsetof(CPUX86State, hflags));
    la_comment("load env.hflags");
    if (intb_njc_enabled() && (!intb_njc_inline_enabled() || call_label)) {
    la_out2(LISA_BEQZ, &cached, &njc_miss);
    la_comment("if TB == NULL");
    HAMT_DMW_FIXIN(cached);

    // 0.4 check TB or compact shadow entry
    // a1 = GPC
    // t1 = next TB
    // t4 = env.hlags
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
    la_out2i(LISA_LD_WU, &t2, &cached, offsetof(TranslationBlock, flags));
    la_out3(LISA_BNE, &t2, &t4, &njc_miss);
#else
    printf("%s:%d TODO\n", __func__, __LINE__); exit(-1);
#endif
    la_out2i(LISA_LOAD_PC, &t2, &cached,
             offsetof(TranslationBlock, pc));

    la_out3(LISA_BNE, &t2, &la_a1, &njc_miss);

    la_comment("check TB.pc");
    la_out2i(LISA_LD_WU, &t2, &cached,
             offsetof(TranslationBlock, cflags));
    la_out2ii(LISA_BSTRPICK_D, &t2, &t2, 18, 18);
    la_out2(LISA_BNEZ, &t2, &njc_miss);
    la_comment("check TB.cflags.TB_INVALID");

    la_out2i(LISA_LD_WU, &t2, &cached,
             offsetof(TranslationBlock, cs_base));
    la_out2i(LISA_LOAD_PC, &t3, &la_env,
             offsetof(CPUX86State, segs[R_CS].base));
    la_out3(LISA_BNE, &t2, &t3, &njc_miss);
    la_comment("check TB.cs_base");

    // a1 = GPC
    // t1 = next TB
    // 0.5 njc lookup hit
    la_out2i(LISA_LD_D, &result, &cached,
             offsetof(TranslationBlock, tc.ptr));

    la_comment("load TB.tc.ptr");
    HAMT_DMW_FIXIN(result);
#ifdef CONFIG_LATX_PRINT
    intb_counter_la_out_inc(t3, t4, fintb_1_njc_hit);
#endif
    if (part == FSHT_CALL_NJC) {
        la_label(*call_label);
        la_out2i(LISA_JIRL, option_tu_hras == 2 ? &la_ra : &la_zero,
                  &la_a0, 0);
        return;
    } else if (call_label) {
        la_out1(LISA_B, call_label);
    } else {
        la_out2i(LISA_JIRL, &la_zero, &result, 0);
    }

    // a1 = GPC
    // t4 = env.flags
//    la_label(njc_miss);
    if (call_label) {
        /* Address lookup clobbers a1. The CALL C fragment syncs it again. */
        la_label(fsht_miss);
        la_out2i(LISA_LOAD_PC, &la_a1, &la_env, offsetof(CPUX86State, eip));
        la_out1(LISA_B, cold_miss);
    }
    la_label_name(njc_miss, "NJC lookup miss");
    if (call_label) {
        la_out2_(lisa_mov, &la_ra, &t1);
    }

#ifdef CONFIG_LATX_PRINT
    if (option_intb_counter) {
    intb_counter_la_out_inc(t2, t3, fintb_1_njc_miss);
    IR2_OPND label_njc_miss_not_zero = la_new_label();
    la_out2(LISA_BNEZ, &cached, &label_njc_miss_not_zero);
    intb_counter_la_out_inc(t2, t3, fintb_1_njc_miss_zero);
    la_label(label_njc_miss_not_zero);
    }
#endif
    }

    } else {
        la_label(fsht_miss);
        la_out2i(LISA_LOAD_PC, &la_a1, &la_env, offsetof(CPUX86State, eip));
        la_out1(LISA_B, cold_miss);
        la_label(njc_miss);
#ifdef CONFIG_LATX_PRINT
        if (option_intb_counter) {
            intb_counter_la_out_inc(t2, t3, fintb_1_njc_miss);
            IR2_OPND not_zero = la_new_label();
            la_out2(LISA_BNEZ, &la_a0, &not_zero);
            intb_counter_la_out_inc(t2, t3, fintb_1_njc_miss_zero);
            la_label(not_zero);
        }
#endif
#ifdef FSHT_LOOKUP_CHECK_FLAGS
        la_out2i(LISA_LD_WU, &t4, &la_env, offsetof(CPUX86State, hflags));
#endif
    }

    latxs_tr_gen_intb_sync_gpc();
    /* Address lookup uses a1 as scratch after NJC misses. */
    la_out2_(lisa_mov, &t0, &la_a1);
    /* ========================== 1. flaged shadow htable =================== */
#if defined(FSHT_LOOKUP_CHECK_FLAGS)
    IR2_OPND fsht_flags_check_ok = la_new_label();
    la_imm(&t3, fshtflag);
    la_out3(LISA_BEQ, &t3, &t4, &fsht_flags_check_ok);
    la_outa(LISA_DUP, 0x0); // invalid inst to cause error
//    la_label(fsht_flags_check_ok);
    la_label_name(fsht_flags_check_ok, "flags check ok");
#endif

    // 1.0 GVA to GPA
    // t0 = GPC
#ifdef CONFIG_SOFTMMU
    if (option_fast_itlb) {
    tr_lookup_code_fast_itlb(&t4, &t0, mmuidx, fsht_miss);
    la_comment("GVA to GPA with fast iTLB");
    } else {
    tr_lookup_code_hva(&t4, &t0, mmuidx, fsht_miss);
    la_comment("GVA to HVA with softTLB");
    }
#else
    la_out2_(lisa_mov, &t4, &t0);
#endif

    // 1.1 load TB from fsht
    // t0 = GPC
    // t4 = physical page (GPA or HVA)
    if (call_label && option_aot_save) {
        unsigned before = td->ir2_asm_nr;
        /* The fixed FSHT mapping is page aligned and below bit 51.
         * Preserve its existing two instructions, including fid zero. */
        g_assert(!(fshtbase & 0xfff) && !(fshtbase >> 51));
        add_curr_rel_entry(td->curr_tb, LOAD_HRAS_FSHT_BASE,
                           before * 4, 2, -1, -1);
        la_imm(&t1, fshtbase);
        g_assert(td->ir2_asm_nr - before == 2);
    } else {
        la_imm(&t1, fshtbase);
    }
    la_comment("load fsht base");
    if (option_fast_itlb) {
    la_out2ii(LISA_BSTRPICK_D, &t2, &t4, 28, 0); // GPA page
    la_out2i (LISA_SLLI_D,     &t2, &t2, 12);
    la_out2ii(LISA_BSTRINS_D,  &t2, &t0, 11, 0); // GPA
    la_comment("phys pc = phys page + pc page offset");
    } else {
    la_out2ii(LISA_BSTRPICK_D,  &t2, &t0, 11, 0);
    la_comment("get pc page offset");
    la_out3  (LISA_ADD_D, &t2, &t4, &t2);
    la_comment("phys pc = phys page + pc page offset");
    }
    la_out2i (LISA_SRLI_D, &t3, &t0, 32);
    la_comment("pc >> 32");
    la_out3  (LISA_XOR, &t2, &t2, &t3);
    la_comment("hash = phys_pc ^ (pc >> 32)");
    latxs_out_fsht_slot(&t1, &t2, fshtbase, code_ptr != NULL);
    HAMT_DMW_FIXIN(t1);
    la_out2i(LISA_LD_D, &t1, &t1, 0);
    la_comment("load TB from fsht");
    la_out2(LISA_BEQZ, &t1, &fsht_miss);
    HAMT_DMW_FIXIN(t1);

    // 1.2 check TB
    // t0 = GPC
    // t1 = next TB
    // t4 = physical page (GPA or HVA)
    la_out2i (LISA_LOAD_PC, &t2, &t1, offsetof(TranslationBlock, pc));
    la_out3  (LISA_BNE,     &t2, &t0, &fsht_miss); // check PC
    la_comment("check TB.pc");
#ifdef CONFIG_SOFTMMU
    if (option_fast_itlb) {
    la_out2i (LISA_LD_D,    &t2, &t1, offsetof(TranslationBlock, page_addr_mix));
    } else {
    la_out2i (LISA_LD_D,    &t2, &t1, offsetof(TranslationBlock, hva));
    }
    la_out3  (LISA_BNE,     &t2, &t4, &fsht_miss); // check physical page
    la_comment("check TB.page_addr");
#endif
    la_out2i (LISA_LD_WU,      &t2, &t1, offsetof(TranslationBlock, cflags));
    la_out2ii(LISA_BSTRPICK_D, &t2, &t2, 18, 18);
    la_out2  (LISA_BNEZ,       &t2, &fsht_miss); // check cflags.TB_INVALID
    la_comment("check TB.cflags.TB_INVALID");
    if (!(fshtflag & HF_CS64_MASK)) {
    la_out2i (LISA_LD_WU,    &t2, &t1, offsetof(TranslationBlock, cs_base));
    la_out2i (LISA_LOAD_PC,    &t3, &la_env, offsetof(CPUX86State, segs[R_CS].base));
    la_out3  (LISA_BNE,        &t2, &t3, &fsht_miss); // check CSBASE
    la_comment("check TB.cs_base");
    }

    // 1.3 fsht lookup hit
    // t1 = next TB
    if (tucpfast_version) {
        int call_offset;

        la_out2i(LISA_ST_D, &la_ra, &la_env,
                 offsetof(CPUX86State, tmparea[0]));
        la_out2i(LISA_ST_D, &t1, &la_env,
                 offsetof(CPUX86State, tmparea[1]));
        la_out2_(lisa_mov, &la_a6, &t1);
        if (call_label) {
            TranslationBlock *tb = td->curr_tb;
            if (option_aot_save) {
                tb_sc_table_offset_jmp(tb, mtcc_id, tu_cp_check_ptr_mmu_user,
                    td->tcg_region_offset, td->ir2_asm_nr << 2, 1,
                    HRAS_TU_CP_CHECK_PTR);
            } else {
                call_offset = (GET_SC_TABLE_OFFSET(mtcc_id,
                    tu_cp_check_ptr_mmu_user, td->tcg_region_offset) -
                    ((ADDR)tb->tc.ptr + (td->ir2_asm_nr << 2))) >> 2;
                latxs_append_ir2_jmp_far(call_offset, 1);
            }
        } else {
            call_offset = (GET_SC_TABLE(mtcc_id, tu_cp_check_ptr_mmu_user) -
                           (ADDR)code_ptr - (td->ir2_asm_nr << 2)) >> 2;
            la_outa(LISA_BL, call_offset);
        }
        la_out2i(LISA_LD_D, &la_ra, &la_env,
                 offsetof(CPUX86State, tmparea[0]));
        la_out2i(LISA_LD_D, &t1, &la_env,
                 offsetof(CPUX86State, tmparea[1]));
        HAMT_DMW_FIXIN(t1);
    }
    la_out2i(LISA_LD_D, &result, &t1, offsetof(TranslationBlock, tc.ptr));
    la_comment("load TB.tc.ptr");
    HAMT_DMW_FIXIN(result);
#ifdef CONFIG_LATX_PRINT
    intb_counter_la_out_inc(t3, t4, fintb_2_fsht_hit);
#endif
#ifdef LATX_PCREL
    la_out2i(LISA_ST_D, &t0, &la_ra,
             offsetof(CPUJumpCacheEntry, pc));
#endif
    HAMT_DMW_FIXOUT(t1);
    la_out2i(LISA_ST_D, &t1, &la_ra,
             offsetof(CPUJumpCacheEntry, tb));

    la_comment("fill into legacy jmp cache");
    if (!call_label) {
        la_out2i(LISA_JIRL, &la_zero, &result, 0);
    }
}

static int gen_latxs_intb_fsht_lookup_legacy_impl(void *code_ptr, int fid,
                                           bool tucpfast_version)
{
#if defined(CONFIG_LATX_PRINT)
    flaged_shadow_hash_table_t *f = &(fsht[fid]);
    uint64_t fshtbase = f->base;
    uint32_t fshtflag = f->flags;

    printf("%s:%d code %p fsht %x base 0x%lx\n", __func__, __LINE__,
            code_ptr, fshtflag, fshtbase);
#endif

    int code_nr = 0;

    latxs_tr_init(NULL);

    TRANSLATION_DATA *td = lsenv->tr_data;

    uint32_t mtcc_id = td->mtcc_id;

    int start = (td->ir2_asm_nr << 2);
    int offset = 0;
    int64_t ins_offset = 0;

    IR2_OPND t0 = latxs_ra_alloc_itemp();
    IR2_OPND t1 = latxs_ra_alloc_itemp();
    IR2_OPND t2 = latxs_ra_alloc_itemp();
    IR2_OPND t3 = latxs_ra_alloc_itemp();
    IR2_OPND t4 = latxs_ra_alloc_itemp();

#ifdef CONFIG_LATX_PRINT
    intb_counter_la_out_inc(t0, t1, fintb_num);
    intb_counter_la_out_inc(t0, t1,  intb_num);
#endif

    /* =============================== 0. njc =============================== */
    IR2_OPND njc_miss = la_new_label();
    IR2_OPND fsht_miss = la_new_label();

    gen_latxs_intb_fsht_lookup_legacy_body(code_ptr, fid, tucpfast_version,
        t0, t1, t2, t3, t4, njc_miss, fsht_miss, NULL, NULL, FSHT_CALL_FULL);

//    la_label(fsht_miss);
    la_label_name(fsht_miss, "Flaged HT lookup miss");
#ifdef CONFIG_LATX_PRINT
    if (option_intb_counter) {
    intb_counter_la_out_inc(t2, t3, fintb_2_fsht_miss);
    IR2_OPND label_fsht_miss_not_zero = la_new_label();
    la_out2(LISA_BNEZ, &t1, &label_fsht_miss_not_zero);
    intb_counter_la_out_inc(t2, t3, fintb_2_fsht_miss_zero);
    la_label(label_fsht_miss_not_zero);
    }
#endif
    /* ========================== 2. helper lookup tb ======================= */

    la_tmp_free(t0);
    la_tmp_free(t1);
    la_tmp_free(t2);
    la_tmp_free(t3);
    la_tmp_free(t4);

    offset = (td->ir2_asm_nr << 2) - start;
#ifdef CONFIG_LATX_OPT_NOFP
    ins_offset = GET_SC_TABLE(mtcc_id, scs_prologue_nofp) - (ADDR)code_ptr - offset;
#else
    ins_offset = GET_SC_TABLE(mtcc_id, scs_prologue) - (ADDR)code_ptr - offset;
#endif
    ins_offset = ins_offset >> 2;
    latxs_append_ir2_jmp_far(ins_offset, 1);
    la_comment("context switch prologue");

    la_out2_(lisa_mov, &la_a0, &la_env);
    latxs_tr_gen_call_to_helper((ADDR)latx_helper_lookup_tb_hashtable,
            LATX_HELPER_LOOKUP_TB_HASHTABLE);
    la_comment("call helper lookup tb");

    offset = (td->ir2_asm_nr << 2) - start;
#ifdef CONFIG_LATX_OPT_NOFP
    ins_offset = GET_SC_TABLE(mtcc_id, scs_epilogue_nofp) - (ADDR)code_ptr - offset;
#else
    ins_offset = GET_SC_TABLE(mtcc_id, scs_epilogue) - (ADDR)code_ptr - offset;
#endif
    ins_offset = ins_offset >> 2;
    latxs_append_ir2_jmp_far(ins_offset, 1);
    la_comment("context switch epilogue");

#ifdef CONFIG_LATX_PRINT
    if (option_intb_counter) {
    uint64_t helper_miss_target = (uint64_t)GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0);
    IR2_OPND label_helper_miss = la_new_label();
    IR2_OPND label_helper_counter_done = la_new_label();
    la_imm(&t0, helper_miss_target);
    la_out3(LISA_BEQ, &t0, &la_v0, &label_helper_miss);

    intb_counter_la_out_inc(t0, t1, fintb_3_helper_hit);
    la_out1(LISA_B, &label_helper_counter_done);

    la_label(label_helper_miss);
    intb_counter_la_out_inc(t0, t1, fintb_3_helper_miss);

    la_label(label_helper_counter_done);
    }
#endif

    HAMT_DMW_FIXIN(la_v0);
    la_out2i(LISA_JIRL, &la_zero, &la_v0, 0);

    code_nr = latxs_tr_ir2_assemble(code_ptr);
    latxs_tr_fini();
    return code_nr;
}

static void gen_latxs_intb_fsht_lookup_compact_body(void *code_ptr, int fid,
        bool tucpfast_version, IR2_OPND t0, IR2_OPND t1, IR2_OPND t2,
        IR2_OPND t3, IR2_OPND t4, IR2_OPND njc_miss, IR2_OPND fsht_miss,
        IR2_OPND *call_label, IR2_OPND *cold_miss,
        FSHTCallPart part, bool call_target)
{
    flaged_shadow_hash_table_t *f = &fsht[fid];
    uint64_t fshtbase = f->base;
    uint32_t __attribute__((unused)) fshtflag = f->flags;
    TRANSLATION_DATA *td = lsenv->tr_data;
    uint32_t mtcc_id = td->mtcc_id;
    IR2_OPND cached = call_label ? la_a0 : t1;
    IR2_OPND result = call_label ? la_a0 : t2;
    IR2_OPND slot = call_label && part != FSHT_CALL_NJC ? t1 : la_ra;
    bool use_njc = intb_njc_enabled() &&
                   (!intb_njc_inline_enabled() || call_label || call_target);
#ifdef CONFIG_SOFTMMU
    int mmuidx = MMU_KNOSMAP_IDX;
    if ((fshtflag & 0x3) == 3) {
        mmuidx = MMU_USER_IDX;
    }
#endif

    if (part != FSHT_CALL_MISS) {
    // 0.2 calculate hash for jmp cache
    // a1 = GPC
    if (use_njc) {
    if (!latxs_tr_gen_compact_njc_slot(&la_a1, &t1, &t2,
            code_ptr ? option_table_addr : option_njc_layout)) {
        latxs_out_jmp_cache_hash(&la_a1, &t1, &t2);
        la_out2i(LISA_LD_D, &t1, &la_env,
                lsenv_offset_of_latx_njc_cache_ptr(lsenv));
        la_out2i(LISA_SLLI_D, &t2, &t2, 5);
        la_out3(LISA_ADD_D, &t1, &t2, &t1);
    }

    if (!call_label || part == FSHT_CALL_NJC) {
        la_out2_(lisa_mov, &la_ra, &t1);
    }
    la_out2i(LISA_LD_D, &cached, &t1, call_target ?
             offsetof(LatxNJCEntry, call_tcptr) : offsetof(LatxNJCEntry, tcptr));
    /* The NJC is per-CPU; its lookup does not need an acquire barrier. */
    la_comment("load TB from jmp cache");
    }

    la_out2i(LISA_LD_WU, &t4, &la_env, offsetof(CPUX86State, hflags));
    la_comment("load env.hflags");
    if (use_njc) {
    la_out2(LISA_BEQZ, &cached, &njc_miss);
    la_comment("if TB == NULL");
    // 0.4 check TB or compact shadow entry
    // a1 = GPC
    // t1 = next TB
    // t4 = env.hlags
    la_out2i(LISA_LD_WU, &t2, &slot,
             offsetof(LatxNJCEntry, flags));
    la_out3(LISA_BNE, &t2, &t4, &njc_miss);
    la_comment("compact CS64 flags check; CS base is ignored");
    la_out2i(LISA_LOAD_PC, &t2, &slot,
             offsetof(LatxNJCEntry, pc));

    la_out3(LISA_BNE, &t2, &la_a1, &njc_miss);
    if (call_label && option_aot_save && !in_aot_translate) {
        la_out2i(LISA_LD_HU, &t2, &slot,
                 offsetof(LatxNJCEntry, observed_call_entry));
        /* The NJC-only TU fragment has no C-miss label. Its existing miss
         * island enters FSHT, which performs the first-observation check. */
        la_out2(LISA_BEQZ, &t2, &njc_miss);
    }

#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
    if (!call_target && aot_tu_enabled() && hamt_enabled() && hamt_mode_dmw() &&
        option_soft_ras && option_rastoptp &&
        option_int_opt == IRQ_CHECK_ASYNC_NOTIFY) {
        la_out2i(LISA_LD_D, &t3, &slot, offsetof(LatxNJCEntry, call_tcptr));
        la_out2(LISA_BEQZ, &t3, &njc_miss);
        la_out3(LISA_BNE, &t3, &cached, &njc_miss);
    }
#endif
    la_comment("check TB.pc");
    // a1 = GPC
    // t1 = next TB
    // 0.5 njc lookup hit
    la_comment("load TB.tc.ptr");
#ifdef CONFIG_LATX_PRINT
    intb_counter_la_out_inc(t3, t4, fintb_1_njc_hit);
#endif
    if (part == FSHT_CALL_NJC) {
        la_label(*call_label);
        la_out2i(LISA_JIRL, option_tu_hras == 2 ? &la_ra : &la_zero,
                  &la_a0, 0);
        return;
    } else if (call_label) {
        la_out1(LISA_B, call_label);
    } else {
        la_out2i(LISA_JIRL, &la_zero, &cached, 0);
    }

    // a1 = GPC
    // t4 = env.flags
//    la_label(njc_miss);
    if (call_label) {
        /* Address lookup clobbers a1. The CALL C fragment syncs it again. */
        la_label(fsht_miss);
        la_out2i(LISA_LOAD_PC, &la_a1, &la_env, offsetof(CPUX86State, eip));
        la_out1(LISA_B, cold_miss);
    }
    la_label_name(njc_miss, "NJC lookup miss");
    if (call_label) {
        la_out2_(lisa_mov, &la_ra, &t1);
    }

#ifdef CONFIG_LATX_PRINT
    if (option_intb_counter) {
    intb_counter_la_out_inc(t2, t3, fintb_1_njc_miss);
    IR2_OPND label_njc_miss_not_zero = la_new_label();
    la_out2(LISA_BNEZ, &cached, &label_njc_miss_not_zero);
    intb_counter_la_out_inc(t2, t3, fintb_1_njc_miss_zero);
    la_label(label_njc_miss_not_zero);
    }
#endif
    }

    } else {
        la_label(fsht_miss);
        la_out2i(LISA_LOAD_PC, &la_a1, &la_env, offsetof(CPUX86State, eip));
        la_out1(LISA_B, cold_miss);
        la_label(njc_miss);
#ifdef CONFIG_LATX_PRINT
        if (option_intb_counter) {
            intb_counter_la_out_inc(t2, t3, fintb_1_njc_miss);
            IR2_OPND not_zero = la_new_label();
            la_out2(LISA_BNEZ, &la_a0, &not_zero);
            intb_counter_la_out_inc(t2, t3, fintb_1_njc_miss_zero);
            la_label(not_zero);
        }
#endif
#ifdef FSHT_LOOKUP_CHECK_FLAGS
        la_out2i(LISA_LD_WU, &t4, &la_env, offsetof(CPUX86State, hflags));
#endif
    }

    /* The fast shared stub is generated after the ordinary one and replaces
     * its entry for user flags when TUCP fast checks are enabled. */
    if (code_ptr && part != FSHT_CALL_MISS && !call_label && !call_target) {
        SET_SC_TABLE(mtcc_id, intb_fsht_after_njc[fid],
                     (ADDR)code_ptr + (td->ir2_asm_nr << 2));
    }
#ifdef FSHT_LOOKUP_CHECK_FLAGS
    la_out2i(LISA_LD_WU, &t4, &la_env, offsetof(CPUX86State, hflags));
#endif
    latxs_tr_gen_intb_sync_gpc();
    /* Address lookup uses a1 as scratch after NJC misses. */
    la_out2_(lisa_mov, &t0, &la_a1);
    /* ========================== 1. flaged shadow htable =================== */
#if defined(FSHT_LOOKUP_CHECK_FLAGS)
    IR2_OPND fsht_flags_check_ok = la_new_label();
    la_imm(&t3, fshtflag);
    la_out3(LISA_BEQ, &t3, &t4, &fsht_flags_check_ok);
    la_outa(LISA_DUP, 0x0); // invalid inst to cause error
//    la_label(fsht_flags_check_ok);
    la_label_name(fsht_flags_check_ok, "flags check ok");
#endif

    // 1.0 GVA to GPA
    // t0 = GPC
#ifdef CONFIG_SOFTMMU
    if (option_fast_itlb) {
    tr_lookup_code_fast_itlb(&t4, &t0, mmuidx, fsht_miss);
    la_comment("GVA to GPA with fast iTLB");
    } else {
    tr_lookup_code_hva(&t4, &t0, mmuidx, fsht_miss);
    la_comment("GVA to HVA with softTLB");
    }
#else
    la_out2_(lisa_mov, &t4, &t0);
#endif

    // 1.1 load TB from fsht
    // t0 = GPC
    // t4 = physical page (GPA or HVA)
    if (call_label && option_aot_save) {
        unsigned before = td->ir2_asm_nr;
        /* The fixed FSHT mapping is page aligned and below bit 51.
         * Preserve its existing two instructions, including fid zero. */
        g_assert(!(fshtbase & 0xfff) && !(fshtbase >> 51));
        add_curr_rel_entry(td->curr_tb, LOAD_HRAS_FSHT_BASE,
                           before * 4, 2, -1, -1);
        la_imm(&t1, fshtbase);
        g_assert(td->ir2_asm_nr - before == 2);
    } else {
        la_imm(&t1, fshtbase);
    }
    la_comment("load fsht base");
    if (option_fast_itlb) {
    la_out2ii(LISA_BSTRPICK_D, &t2, &t4, 28, 0); // GPA page
    la_out2i (LISA_SLLI_D,     &t2, &t2, 12);
    la_out2ii(LISA_BSTRINS_D,  &t2, &t0, 11, 0); // GPA
    la_comment("phys pc = phys page + pc page offset");
    } else {
    la_out2ii(LISA_BSTRPICK_D,  &t2, &t0, 11, 0);
    la_comment("get pc page offset");
    la_out3  (LISA_ADD_D, &t2, &t4, &t2);
    la_comment("phys pc = phys page + pc page offset");
    }
    la_out2i (LISA_SRLI_D, &t3, &t0, 32);
    la_comment("pc >> 32");
    la_out3  (LISA_XOR, &t2, &t2, &t3);
    la_comment("hash = phys_pc ^ (pc >> 32)");
    latxs_out_fsht_slot(&t1, &t2, fshtbase, code_ptr != NULL);
    HAMT_DMW_FIXIN(t1);
    la_out2i(LISA_LD_D, &t1, &t1, 0);
    la_comment("load TB from fsht");
    la_out2(LISA_BEQZ, &t1, &fsht_miss);
    HAMT_DMW_FIXIN(t1);

    // 1.2 check TB
    // t0 = GPC
    // t1 = next TB
    // t4 = physical page (GPA or HVA)
    la_out2i (LISA_LOAD_PC, &t2, &t1, offsetof(TranslationBlock, pc));
    la_out3  (LISA_BNE,     &t2, &t0, &fsht_miss); // check PC
    la_comment("check TB.pc");
#ifdef CONFIG_SOFTMMU
    if (option_fast_itlb) {
    la_out2i (LISA_LD_D,    &t2, &t1, offsetof(TranslationBlock, page_addr_mix));
    } else {
    la_out2i (LISA_LD_D,    &t2, &t1, offsetof(TranslationBlock, hva));
    }
    la_out3  (LISA_BNE,     &t2, &t4, &fsht_miss); // check physical page
    la_comment("check TB.page_addr");
#endif
    la_out2i (LISA_LD_WU,      &t2, &t1, offsetof(TranslationBlock, cflags));
    la_out2ii(LISA_BSTRPICK_D, &t2, &t2, 18, 18);
    la_out2  (LISA_BNEZ,       &t2, &fsht_miss); // check cflags.TB_INVALID
    la_comment("check TB.cflags.TB_INVALID");

    // 1.3 fsht lookup hit
    // t1 = next TB
    if (tucpfast_version) {
        int call_offset;

        la_out2i(LISA_ST_D, &la_ra, &la_env,
                 offsetof(CPUX86State, tmparea[0]));
        la_out2i(LISA_ST_D, &t1, &la_env,
                 offsetof(CPUX86State, tmparea[1]));
        la_out2_(lisa_mov, &la_a6, &t1);
        if (call_label) {
            TranslationBlock *tb = td->curr_tb;
            if (option_aot_save) {
                tb_sc_table_offset_jmp(tb, mtcc_id, tu_cp_check_ptr_mmu_user,
                    td->tcg_region_offset, td->ir2_asm_nr << 2, 1,
                    HRAS_TU_CP_CHECK_PTR);
            } else {
                call_offset = (GET_SC_TABLE_OFFSET(mtcc_id,
                    tu_cp_check_ptr_mmu_user, td->tcg_region_offset) -
                    ((ADDR)tb->tc.ptr + (td->ir2_asm_nr << 2))) >> 2;
                latxs_append_ir2_jmp_far(call_offset, 1);
            }
        } else {
            call_offset = (GET_SC_TABLE(mtcc_id, tu_cp_check_ptr_mmu_user) -
                           (ADDR)code_ptr - (td->ir2_asm_nr << 2)) >> 2;
            la_outa(LISA_BL, call_offset);
        }
        la_out2i(LISA_LD_D, &la_ra, &la_env,
                 offsetof(CPUX86State, tmparea[0]));
        la_out2i(LISA_LD_D, &t1, &la_env,
                 offsetof(CPUX86State, tmparea[1]));
        HAMT_DMW_FIXIN(t1);
    }
#ifdef CONFIG_LATX_PRINT
    intb_counter_la_out_inc(t3, t4, fintb_2_fsht_hit);
#endif
    la_out2i(LISA_LD_WU, &t3, &t1, offsetof(TranslationBlock, misc_flags));
    la_out2i(LISA_ANDI, &t3, &t3, AOT_BLOCK_OBSERVED_CALL_ENTRY);
    if (call_label && option_aot_save && !in_aot_translate) {
        /* fsht_miss restores the target PC before entering the CALL helper. */
        la_out2(LISA_BEQZ, &t3, &fsht_miss);
    }
    if (use_njc) {
        la_out2i(LISA_ST_W, &t3, &la_ra,
                 offsetof(LatxNJCEntry, observed_call_entry));
        la_out2i(LISA_ST_D, &la_zero, &la_ra, offsetof(LatxNJCEntry, call_tcptr));
        la_out2i(LISA_LOAD_PC, &t0, &la_env, offsetof(CPUX86State, eip));
        la_out2i(LISA_ST_D, &t0, &la_ra, offsetof(LatxNJCEntry, pc));
        la_out2i(LISA_LD_WU, &t3, &t1, offsetof(TranslationBlock, flags));
        la_out2i(LISA_ST_W, &t3, &la_ra, offsetof(LatxNJCEntry, flags));
    }
    la_out2i(LISA_LD_D, &result, &t1, offsetof(TranslationBlock, tc.ptr));
    HAMT_DMW_FIXIN(result);
    if (use_njc) {
        la_out2i(LISA_ST_D, &result, &la_ra, offsetof(LatxNJCEntry, tcptr));
    }
#ifdef CONFIG_LATX_TU
    {
        IR2_OPND have_call_target = la_new_label();
        IR2_OPND ordinary_target = la_new_label();
        IR2_OPND fsht_done = la_new_label();
        la_out2i(LISA_LD_BU, &t3, &t1, offsetof(TranslationBlock, bool_flags));
        la_out2i(LISA_ANDI, &t3, &t3, IS_AOT_TB | IS_TU_TB);
        la_out2i(LISA_ADDI_D, &t4, &la_zero, IS_AOT_TB | IS_TU_TB);
        la_out3(LISA_BNE, &t3, &t4, &ordinary_target);
        la_out2i(LISA_LD_D, &t4, &t1, offsetof(TranslationBlock, s_data));
        HAMT_DMW_FIXIN(t4);
        la_out2i(LISA_LD_D, &t3, &t4, offsetof(struct separated_data, aot_native_call_entry));
        la_out2i(LISA_LD_D, &t4, &t4,
                 offsetof(struct separated_data, aot_direct_native_entry));
        la_out2(LISA_BNEZ, &t3, &have_call_target);
        la_out2(LISA_BEQZ, &t4, &ordinary_target);
        if (use_njc) {
            la_out2i(LISA_ST_H, &la_zero, &la_ra,
                     offsetof(LatxNJCEntry, direct_entry_off));
            la_out2i(LISA_ST_D, &la_zero, &la_ra, offsetof(LatxNJCEntry, tcptr));
        }
#ifdef CONFIG_SOFTMMU
        if (!call_target && aot_tu_enabled() && hamt_enabled() && hamt_mode_dmw() &&
            option_soft_ras && option_rastoptp &&
            option_int_opt == IRQ_CHECK_ASYNC_NOTIFY) {
            la_out2(LISA_BNEZ, &t4, &fsht_miss);
        }
#endif
        la_out2(LISA_BNEZ, &t4, &fsht_done);
        la_label(ordinary_target);
        if (use_njc) {
            la_out2_(lisa_mov, &t4, &la_zero);
        }
        la_out2_(lisa_mov, &t3, &result);
        la_label(have_call_target);
        HAMT_DMW_FIXIN(t3);
        if (use_njc) {
            la_out3(LISA_SLTU, &t4, &la_zero, &t4);
            la_out2i(LISA_SLLI_D, &t4, &t4, 5);
            la_out2i(LISA_ST_H, &t4, &la_ra,
                     offsetof(LatxNJCEntry, direct_entry_off));
            la_out2i(LISA_ST_D, &t3, &la_ra, offsetof(LatxNJCEntry, call_tcptr));
        }
        if (call_target) la_out2_(lisa_mov, &result, &t3);
#ifdef CONFIG_SOFTMMU
        if (!call_target && aot_tu_enabled() && hamt_enabled() && hamt_mode_dmw() &&
            option_soft_ras && option_rastoptp &&
            option_int_opt == IRQ_CHECK_ASYNC_NOTIFY) {
            /* eip was synced before the FSHT walk; the C miss can now
             * unwind an active LLVM caller without changing guest RSP. */
            la_out3(LISA_BNE, &t3, &result, &fsht_miss);
        }
#endif
        la_label(fsht_done);
    }
#else
    if (use_njc) {
        la_out2i(LISA_ST_D, &result, &la_ra, offsetof(LatxNJCEntry, call_tcptr));
    }
#endif
    la_comment("compact FSHT hit; fill saved Latx NJC slot in ra");
    if (!call_label) {
        la_out2i(LISA_JIRL, &la_zero, &result, 0);
    }
}

static int gen_latxs_intb_fsht_lookup_compact_impl(void *code_ptr, int fid,
                                           bool tucpfast_version, bool call_target)
{
#if defined(CONFIG_LATX_PRINT)
    flaged_shadow_hash_table_t *f = &(fsht[fid]);
    uint64_t fshtbase = f->base;
    uint32_t __attribute__((unused)) fshtflag = f->flags;

    printf("%s:%d code %p fsht %x base 0x%lx\n", __func__, __LINE__,
            code_ptr, fshtflag, fshtbase);
#endif

    int code_nr = 0;

    latxs_tr_init(NULL);

    TRANSLATION_DATA *td = lsenv->tr_data;

    uint32_t mtcc_id = td->mtcc_id;

    int start = (td->ir2_asm_nr << 2);
    int offset = 0;
    int64_t ins_offset = 0;

    IR2_OPND t0 = latxs_ra_alloc_itemp();
    IR2_OPND t1 = latxs_ra_alloc_itemp();
    IR2_OPND t2 = latxs_ra_alloc_itemp();
    IR2_OPND t3 = latxs_ra_alloc_itemp();
    IR2_OPND t4 = latxs_ra_alloc_itemp();

#ifdef CONFIG_LATX_PRINT
    intb_counter_la_out_inc(t0, t1, fintb_num);
    intb_counter_la_out_inc(t0, t1,  intb_num);
#endif

    /* =============================== 0. njc =============================== */
    IR2_OPND njc_miss = la_new_label();
    IR2_OPND fsht_miss = la_new_label();

    gen_latxs_intb_fsht_lookup_compact_body(code_ptr, fid, tucpfast_version,
        t0, t1, t2, t3, t4, njc_miss, fsht_miss, NULL, NULL,
        FSHT_CALL_FULL, call_target);

//    la_label(fsht_miss);
    la_label_name(fsht_miss, "Flaged HT lookup miss");
#ifdef CONFIG_LATX_PRINT
    if (option_intb_counter) {
    intb_counter_la_out_inc(t2, t3, fintb_2_fsht_miss);
    IR2_OPND label_fsht_miss_not_zero = la_new_label();
    la_out2(LISA_BNEZ, &t1, &label_fsht_miss_not_zero);
    intb_counter_la_out_inc(t2, t3, fintb_2_fsht_miss_zero);
    la_label(label_fsht_miss_not_zero);
    }
#endif
    /* ========================== 2. helper lookup tb ======================= */

    la_tmp_free(t0);
    la_tmp_free(t1);
    la_tmp_free(t2);
    la_tmp_free(t3);
    la_tmp_free(t4);

    offset = (td->ir2_asm_nr << 2) - start;
#ifdef CONFIG_LATX_OPT_NOFP
    ins_offset = GET_SC_TABLE(mtcc_id, scs_prologue_nofp) - (ADDR)code_ptr - offset;
#else
    ins_offset = GET_SC_TABLE(mtcc_id, scs_prologue) - (ADDR)code_ptr - offset;
#endif
    ins_offset = ins_offset >> 2;
    latxs_append_ir2_jmp_far(ins_offset, 1);
    la_comment("context switch prologue");

    la_out2_(lisa_mov, &la_a0, &la_env);
    if (call_target) {
        /* This lookup is generated as runtime static code, not saved in AOT. */
        la_imm(&la_ra, (ADDR)latx_helper_lookup_tb_hashtable_call);
        la_out1_(lisa_call, &la_ra);
    } else {
        latxs_tr_gen_call_to_helper((ADDR)latx_helper_lookup_tb_hashtable,
                LATX_HELPER_LOOKUP_TB_HASHTABLE);
    }
    la_comment("call helper lookup tb");

    offset = (td->ir2_asm_nr << 2) - start;
#ifdef CONFIG_LATX_OPT_NOFP
    ins_offset = GET_SC_TABLE(mtcc_id, scs_epilogue_nofp) - (ADDR)code_ptr - offset;
#else
    ins_offset = GET_SC_TABLE(mtcc_id, scs_epilogue) - (ADDR)code_ptr - offset;
#endif
    ins_offset = ins_offset >> 2;
    latxs_append_ir2_jmp_far(ins_offset, 1);
    la_comment("context switch epilogue");

#ifdef CONFIG_LATX_PRINT
    if (option_intb_counter) {
    uint64_t helper_miss_target = (uint64_t)GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0);
    IR2_OPND label_helper_miss = la_new_label();
    IR2_OPND label_helper_counter_done = la_new_label();
    la_imm(&t0, helper_miss_target);
    la_out3(LISA_BEQ, &t0, &la_v0, &label_helper_miss);

    intb_counter_la_out_inc(t0, t1, fintb_3_helper_hit);
    la_out1(LISA_B, &label_helper_counter_done);

    la_label(label_helper_miss);
    intb_counter_la_out_inc(t0, t1, fintb_3_helper_miss);

    la_label(label_helper_counter_done);
    }
#endif

    HAMT_DMW_FIXIN(la_v0);
    la_out2i(LISA_JIRL, &la_zero, &la_v0, 0);

    code_nr = latxs_tr_ir2_assemble(code_ptr);
    latxs_tr_fini();
    return code_nr;
}


static bool use_compact_fsht(int fid)
{
    return latx_jmp_cache_compact_enabled() &&
           (fsht[fid].flags & HF_CS64_MASK);
}

static void gen_latxs_intb_fsht_call_emit(int fid, bool tucpfast_version,
        void *miss_label, void *call_label, void *jump_target,
        IR2_OPND *tu_njc_miss, FSHTCallPart part,
        IR2_OPND *save_scratch, const IR2_OPND *reuse_scratch)
{
    /* Reused registers belong to the source fragment, not this allocator. */
    IR2_OPND scratch[5];
    for (unsigned i = 0; i < 5; i++) {
        scratch[i] = reuse_scratch ? reuse_scratch[i] : latxs_ra_alloc_itemp();
        if (save_scratch) {
            save_scratch[i] = scratch[i];
        }
    }
    IR2_OPND t0 = scratch[0], t1 = scratch[1], t2 = scratch[2];
    IR2_OPND t3 = scratch[3], t4 = scratch[4];
    IR2_OPND njc_miss = tu_njc_miss ? *tu_njc_miss : la_new_label();
    IR2_OPND fsht_miss = la_new_label();

    /* A0 is the final CALL target. Keep the NJC slot in t1 until a miss;
     * only then does FSHT need it in ra for page checks and cache fill. */
    if (use_compact_fsht(fid)) {
        gen_latxs_intb_fsht_lookup_compact_body(NULL, fid, tucpfast_version,
            t0, t1, t2, t3, t4, njc_miss, fsht_miss,
            call_label, miss_label, part, true);
    } else {
        gen_latxs_intb_fsht_lookup_legacy_body(NULL, fid, tucpfast_version,
            t0, t1, t2, t3, t4, njc_miss, fsht_miss,
            call_label, miss_label, part);
    }
    *(IR2_OPND *)jump_target = la_a0;
    if (part == FSHT_CALL_MISS) {
        la_out1(LISA_B, (IR2_OPND *)call_label);
    } else if (part != FSHT_CALL_NJC) {
        la_label(*(IR2_OPND *)call_label);
        la_out2i(LISA_JIRL, option_tu_hras == 2 ? &la_ra : &la_zero, &la_a0, 0);
    }
    if (!reuse_scratch) {
        for (unsigned i = 5; i > 0; i--) {
            latxs_ra_free_temp(&scratch[i - 1]);
        }
    }
}

void gen_latxs_intb_fsht_call(int fid, bool tucpfast_version,
        void *miss_label, void *call_label, void *jump_target)
{
    gen_latxs_intb_fsht_call_emit(fid, tucpfast_version, miss_label,
        call_label, jump_target, NULL, FSHT_CALL_FULL, NULL, NULL);
}

void gen_latxs_intb_fsht_call_miss(int fid, bool tucpfast_version,
        void *njc_miss, void *miss_label, void *call_label)
{
    IR2_OPND target;

    gen_latxs_intb_fsht_call_emit(fid, tucpfast_version, miss_label,
        call_label, &target, njc_miss, FSHT_CALL_MISS, NULL, NULL);
}

void gen_latxs_intb_fsht_call_njc(int fid, void *njc_miss,
        void *call_label, void *jump_target)
{
    gen_latxs_intb_fsht_call_emit(fid, false, NULL,
        call_label, jump_target, njc_miss, FSHT_CALL_NJC, NULL, NULL);
}

void gen_latxs_intb_fsht_call_njc_save(int fid, void *njc_miss,
        void *call_label, void *jump_target, void *scratch)
{
    gen_latxs_intb_fsht_call_emit(fid, false, NULL,
        call_label, jump_target, njc_miss, FSHT_CALL_NJC, scratch, NULL);
}

void gen_latxs_intb_fsht_call_miss_saved(int fid, bool tucpfast_version,
        void *njc_miss, void *miss_label, void *call_label, const void *scratch)
{
    IR2_OPND target;

    /* The cold fragment may be emitted after unrelated members. Its input
     * registers belong to the source NJC, not to that later allocator state. */
    gen_latxs_intb_fsht_call_emit(fid, tucpfast_version, miss_label,
        call_label, &target, njc_miss, FSHT_CALL_MISS, NULL, scratch);
}

int gen_latxs_intb_fsht_lookup(void *code_ptr, int fid)
{
    if (use_compact_fsht(fid)) {
        return gen_latxs_intb_fsht_lookup_compact_impl(code_ptr, fid, false, false);
    }
    return gen_latxs_intb_fsht_lookup_legacy_impl(code_ptr, fid, false);
}

int gen_latxs_intb_fsht_lookup_tucpfast(void *code_ptr, int fid)
{
    if (use_compact_fsht(fid)) {
        return gen_latxs_intb_fsht_lookup_compact_impl(code_ptr, fid, true, false);
    }
    return gen_latxs_intb_fsht_lookup_legacy_impl(code_ptr, fid, true);
}

int gen_latxs_intb_fsht_call_lookup_tucpfast(void *code_ptr, int fid)
{
    g_assert(use_compact_fsht(fid));
    return gen_latxs_intb_fsht_lookup_compact_impl(code_ptr, fid, true, true);
}
