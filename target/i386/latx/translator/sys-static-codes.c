/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "cpu.h"
#include "lsenv.h"
#include "latx-config.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "latx-perfmap.h"
#include "latx-static-codes.h"
#include "latx-intb-sys.h"
#include "latx-tr.h"
#include "latx-sys-hamt-dmw.h"
#include "latx-hack.h"
#include "latx-shadow-htable.h"
#include "accel/tcg/tb-jmp-cache.h"
#include "qemu/cacheinfo.h"

typedef int (*gen_cp_check_func)(void *code_ptr,
        int n, int mmu, int mode, int ptw);
int gen_latxs_cross_page_check(void *code_ptr,
        int n, int mmu, int mode, int ptw);
int gen_latxs_cross_page_check_ptw(void *code_ptr, int mmu);

latx_sc_table_t *latx_sc_table;
static latx_sc_table_t __latx_sc_table[MTCC_NR];
static void __attribute__((__constructor__)) latxs_sc_init(void)
{
    latx_sc_table = __latx_sc_table;
}

#ifdef CONFIG_LATX_PRINT
#define LATXS_DUMP_STATIC_CODES_INFO(str, ...) do {    \
    fprintf(stderr, str, __VA_ARGS__);              \
} while (0)
#else
#define LATXS_DUMP_STATIC_CODES_INFO(str, ...) do {    \
    if (option_dump) {                                  \
        fprintf(stderr, str, __VA_ARGS__);              \
    }                                                   \
} while (0)
#endif

/*
 * When generating prologue/epilogue in system-mode,
 * the vCPU thread is not created yet.
 *
 * The lsenv is initialized at the begining of vCPU thread.
 *
 * But here we need the lsenv to generate some static
 * native codes. So we use a temp lsenv.
 */

static ENV latxs_lsenv_tmp;
static TRANSLATION_DATA latxs_tr_data_tmp;
static CPUX86State latxs_env_tmp;

int latxs_tr_ready;

static void latxs_set_lsenv_tmp(uint32_t mtcc_id)
{
    memset(&latxs_lsenv_tmp, 0, sizeof(ENV));
    memset(&latxs_tr_data_tmp, 0, sizeof(TRANSLATION_DATA));
    memset(&latxs_env_tmp, 0, sizeof(CPUX86State));

    lsenv = &latxs_lsenv_tmp;
    lsenv->cpu_state = &latxs_env_tmp;
    lsenv->tr_data = &latxs_tr_data_tmp;
    lsenv->tr_data->mtcc_id = mtcc_id;

    latxs_tr_ready = 1;
}

static int gen_latxs_sc_prologue(void *code_ptr)
{
    int i = 0;
    const int extra_space = 40;

    latxs_tr_begin(code_ptr);

    if (hamt_enabled()) { /* hamt swtich into DMW mode */
        latxs_tr_dmw_in_fix_mem(&la_sp);
        latxs_tr_dmw_in_fix_mem(&la_a0);
        latxs_tr_dmw_in_fix_mem(&la_a1);
        latxs_tr_dmw_in_fix_asid_tmp(&la_a1);
    }

    /* 1. save DBT's context in stack, so allocate space on the stack */
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, -256);

    /* 1.1 save callee-saved registers. s0-s7 ($23-$31) */
    IR2_OPND reg = la_inv;
    for (i = 0; i <= 8; ++i) {
        reg = la_new_gpr(i + 23);
        la_out2i(LISA_ST_D, &reg, &la_sp, extra_space + (i << 3));
    }

    /* 1.2 save fp($22), and ra($1) */
    la_out2i(LISA_ST_D, &la_fp, &la_sp, extra_space + 72);
    la_out2i(LISA_ST_D, &la_ra, &la_sp, extra_space + 80);

    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&la_sp);
    }

    /* 2. set up native context */
    la_out3(LISA_OR, &la_env, &la_a1, &la_zero);

    /* 2.1 load x86 mapping registers */
    int load_top = !option_soft_fpu;
#ifdef CONFIG_LATX_OPT_NOFP
    latxs_tr_load_registers_from_env(0xffffffff, 0x00, load_top,
                                     0x0, 0x00);
#else
    if (option_soft_fpu_fast) {
        latxs_tr_load_registers_from_env(0xffffffff, 0, load_top,
                                         0xffffffff, 0x00);

        IR2_OPND mode_fpu = la_new_itmp();
        IR2_OPND label_fpu = la_new_label();

        la_out2i(LISA_LD_BU, &mode_fpu, &la_env, lsenv_offset_of_mode_fpu(lsenv));
        la_out3(LISA_BNE, &mode_fpu, &la_zero, &label_fpu);

        latxs_tr_load_fprs_from_env(0xff, load_top);

        la_label(label_fpu);
        la_tmp_free(mode_fpu);
    } else {
        latxs_tr_load_registers_from_env(0xffffffff, 0xff, load_top,
                                         0xffffffff, 0x00);
    }
#endif

#ifndef CONFIG_LATX_OPT_EFLAGS
    latxs_tr_load_eflags();
#endif
 
    /* 3. jump to native code address (saved in a0) */
    la_out2i(LISA_JIRL, &la_zero, &la_a0, 0);

    la_out_nop();

    i = latxs_tr_end(code_ptr);
    return i;
}
static int gen_latxs_sc_epilogue(void *code_ptr)
{
    int i = 0;
    const int extra_space = 40;

    latxs_tr_begin(code_ptr);

    /*
     * by default set v0 to zero.
     * sc_table.cs_native_to_bt_ret_0 points here
     */
    IR2_OPND label_epi = la_new_label();
    la_out3(LISA_OR, &la_v0, &la_zero, &la_zero);
    la_out1(LISA_B, &label_epi);

    /*
     * 1. set return value from a6
     * sc_table.cs_native_to_bt_ret_n3
     * sc_table.cs_native_to_bt_ret_n1
     * sc_table.cs_native_to_bt_ret_n0
     */
    IR2_OPND tb_ptr_opnd = latxs_ra_alloc_dbt_arg1();
    la_out2i(LISA_ORI, &la_v0, &tb_ptr_opnd, 3); // n3
    la_out1(LISA_B, &label_epi);
    la_out2i(LISA_ORI, &la_v0, &tb_ptr_opnd, 1); // n1
    la_out1(LISA_B, &label_epi);
    la_out2i(LISA_ORI, &la_v0, &tb_ptr_opnd, 0); // n0
    /*
     *  0:  or   a0, zero, zero  // ret_0
     *  4:  b    6
     *  8:  ori  a0, tb, 3     // ret_n3
     * 12:  b    4
     * 16:  ori  a0, tb, 1     // ret_n1
     * 20:  b    2
     * 24:  ori  a0, tb, 0     // ret_n0
     * ------------------------
     * epilogue context switch
     * ------------------------
     */

    la_label_name(label_epi, "epilogue : native to bt");
    if (hamt_enabled()) latxs_tr_dmw_out_fix_mem(&la_v0);
    /* 3. save x86 mapping registers */
    int save_top = (!option_soft_fpu) ? 1 : 0;
#ifdef CONFIG_LATX_OPT_NOFP
    latxs_tr_save_registers_to_env(0xffffffff, 0x00, save_top,
                                   0x0, 0x00);
#else
    if (option_soft_fpu_fast) {
        latxs_tr_save_registers_to_env(0xffffffff, 0, save_top,
                                       0xffffffff, 0x00);

        IR2_OPND mode_fpu = la_new_itmp();
        IR2_OPND label_fpu = la_new_label();

        la_out2i(LISA_LD_BU, &mode_fpu, &la_env, lsenv_offset_of_mode_fpu(lsenv));
        la_out3(LISA_BNE, &mode_fpu, &la_zero, &label_fpu);

        latxs_tr_save_fprs_to_env(0xff, save_top);

        la_label(label_fpu);
        la_tmp_free(mode_fpu);
    } else {
        latxs_tr_save_registers_to_env(0xffffffff, 0xff, save_top,
                                       0xffffffff, 0x00);
    }
#endif

#ifndef CONFIG_LATX_OPT_EFLAGS
    latxs_tr_save_eflags();
#endif

    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_mem(&la_sp);
    }

    /* 4. restore bt's context */
    /* 4.1 restore fp($22) and ra($1) */
    la_out2i(LISA_LD_D, &la_ra, &la_sp, extra_space + 80);
    la_out2i(LISA_LD_D, &la_fp, &la_sp, extra_space + 72);
    /* 4.2 restore callee-saved registers. s0-s7 ($16-$23) */
    IR2_OPND reg = la_inv;
    for (i = 0; i <= 8; ++i) {
        reg = la_new_gpr(i + 23);
        la_out2i(LISA_LD_D, &reg, &la_sp, extra_space + (i << 3));
    }
    /* 4.4 restore sp */
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, 256);

    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&la_sp);
        latxs_tr_dmw_out_fix_asid(NULL);
    }

    /* 5 return */
    la_out2i(LISA_JIRL, &la_zero, &la_ra, 0);

    i = latxs_tr_end(code_ptr);
    return i;
}

int target_latxs_static_codes(void *code_base, uint32_t mtcc_id)
{
    int code_nr = 0;
    int code_nr_all = 0;
    void *code_ptr = code_base;
    uint64_t code_aln = 0;

    latxs_set_lsenv_tmp(mtcc_id);

#ifdef CONFIG_LATX_PRINT
    printf("%s code %p mtcc %d\n", __func__, code_base, mtcc_id);
#endif

#define LATXS_GEN_STATIC_CODES(name, genfn, ...) do {   \
    code_nr = genfn(__VA_ARGS__);                       \
    code_nr_all += code_nr;                             \
    latx_perfmap_insert(code_ptr, code_nr << 2, \
            mtcc_id ? name "_R1" : name "_R0");  \
    code_ptr = code_base + (code_nr_all << 2);          \
    code_aln = (uint64_t)code_ptr;  \
    if (code_aln & 0x3f) {  \
        code_aln += 64;     \
        code_aln &= ~0x3f;  \
        code_nr_all += (code_aln - (uint64_t)code_ptr) >> 2;    \
        code_ptr = (void*)code_aln; \
    }   \
} while (0)

    /* epilogue */
    SET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0, (ADDR)code_ptr);
    SET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n3, (ADDR)code_ptr + 8);
    SET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n1, (ADDR)code_ptr + 16);
    SET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n0, (ADDR)code_ptr + 24);
    /*
     *  0:  or   a0, zero, zero  // ret_0
     *  4:  b    3
     *  8:  ori  a0, tb, 3     // ret_n3
     * 12:  b    3
     * 16:  ori  a0, tb, 1     // ret_n1
     * 20:  b    3
     * 24:  ori  a0, tb, 0     // ret_n0
     * ------------------------
     * epilogue context switch
     * ------------------------
     */
    LATXS_GEN_STATIC_CODES("latxs_epilogue",
            gen_latxs_sc_epilogue, code_ptr);
    LATXS_DUMP_STATIC_CODES_INFO("latxs epilogue ret_0: %p\n",
            (void *)GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0));
    LATXS_DUMP_STATIC_CODES_INFO("latxs epilogue ret_n3: %p\n",
            (void *)GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n3));
    LATXS_DUMP_STATIC_CODES_INFO("latxs epilogue ret_n1: %p\n",
            (void *)GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n1));
    LATXS_DUMP_STATIC_CODES_INFO("latxs epilogue ret_n0: %p\n",
            (void *)GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n0));

    /* prologue */
    /*
     * For signal interrupt optimization, we need to check interrupt
     * pending before jump to TB's native codes. If there is an
     * interrupt pending, we should jump to epilogue to return
     * back to BT context. So we need to know the address of
     * context switch native to bt here.
     */
    SET_SC_TABLE(mtcc_id, cs_bt_to_native, (ADDR)code_ptr);
    LATXS_GEN_STATIC_CODES("latxs_prologue",
            gen_latxs_sc_prologue, code_ptr);
    LATXS_DUMP_STATIC_CODES_INFO("latxs prologue: %p\n",
            (void *)GET_SC_TABLE(mtcc_id, cs_bt_to_native));

    if (scs_enabled()) {
        SET_SC_TABLE(mtcc_id, scs_prologue, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_static_prologue",
                gen_latxs_scs_prologue_cfg, code_ptr, default_helper_cfg);
        LATXS_DUMP_STATIC_CODES_INFO(
                "latxs SCS: static CS prologue %p\n",
                (void *)GET_SC_TABLE(mtcc_id, scs_prologue));
        
        SET_SC_TABLE(mtcc_id, scs_prologue_nofp, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_static_prologue_nofp",
                gen_latxs_scs_prologue_cfg_nofp, code_ptr, nofp_helper_cfg);
        LATXS_DUMP_STATIC_CODES_INFO(
                "latxs SCS: static CS prologue nofp %p\n",
                (void *)GET_SC_TABLE(mtcc_id, scs_prologue_nofp));

        SET_SC_TABLE(mtcc_id, scs_epilogue, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_static_epilogue",
                gen_latxs_scs_epilogue_cfg, code_ptr, default_helper_cfg);
        LATXS_DUMP_STATIC_CODES_INFO(
                "latxs SCS: static CS epilogue %p\n",
                (void *)GET_SC_TABLE(mtcc_id, scs_epilogue));
        
        SET_SC_TABLE(mtcc_id, scs_epilogue_nofp, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_static_epilogue_nofp",
                gen_latxs_scs_epilogue_cfg_nofp, code_ptr, nofp_helper_cfg);
        LATXS_DUMP_STATIC_CODES_INFO(
                "latxs SCS: static CS epilogue nofp %p\n",
                (void *)GET_SC_TABLE(mtcc_id, scs_epilogue_nofp));
    }

    if (intb_link_enable()) {
        SET_SC_TABLE(mtcc_id, intb_lookup, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_intb_lookup",
                gen_latxs_intb_lookup, code_ptr);
        LATXS_DUMP_STATIC_CODES_INFO(
                "latxs intb lookup %p\n",
                (void *)GET_SC_TABLE(mtcc_id, intb_lookup));

        if (latx_jmp_cache_compact_enabled() &&
            !intb_njc_inline_enabled()) {
            SET_SC_TABLE(mtcc_id, intb_lookup_compact, (ADDR)code_ptr);
            LATXS_GEN_STATIC_CODES("latxs_intb_lookup_compact",
                    gen_latxs_intb_lookup_compact, code_ptr);
            LATXS_DUMP_STATIC_CODES_INFO(
                    "latxs compact intb lookup %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, intb_lookup_compact));
        } else {
            SET_SC_TABLE(mtcc_id, intb_lookup_compact,
                    GET_SC_TABLE(mtcc_id, intb_lookup));
        }

        if (option_flaged_sht) {
            int index = 0;
            uint32_t flags = 0;
            FSHT_FOREACH_FLAGS(index, flags) {
                int do_gen_fsht = 0;

                if (qemu_tcg_mtcc_enabled()) {
                    if ((mtcc_id == 1 && (flags & 0x3) == 3) ||
                        (mtcc_id == 0 && (flags & 0x3) == 0))
                    {
                        do_gen_fsht = 1;
                    }
                } else {
                    do_gen_fsht = 1;
                }

                if (do_gen_fsht) {
#if defined(CONFIG_LATX_PRINT)
                    printf("%s:%d mtcc %d intb fsht %d lookup for flags %x\n",
                            __func__, __LINE__,
                            mtcc_id, index, flags);
#endif
                    SET_SC_TABLE(mtcc_id, intb_fsht_lookup[index], (ADDR)code_ptr);
                    LATXS_GEN_STATIC_CODES("latxs_intb_fsht_lookup",
                            gen_latxs_intb_fsht_lookup, code_ptr, index);
                    LATXS_DUMP_STATIC_CODES_INFO(
                            "latxs intb fsht %d lookup %p\n",
                            index,
                            (void *)GET_SC_TABLE(mtcc_id, intb_fsht_lookup[index]));

                }
            }
        }
    }

#ifdef CONFIG_SOFTMMU
    if (cp_enabled() || cp_tb_enabled()) {
        int have_user = 0;
        int have_knosmap = 0;
        if (qemu_tcg_mtcc_enabled()) {
            if (mtcc_id == 0) have_knosmap = 1;
            if (mtcc_id == 1) have_user = 1;
        } else {
            have_knosmap = 1;
            have_user = 1;
        }

        gen_cp_check_func cp_fn = gen_latxs_cross_page_check;

#define CP_INTCHECK_SIZE    8

        if (have_user) {
            SET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n0_p01_mmu_user",
                    cp_fn, code_ptr, 0, MMU_USER_IDX, 3, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n0_p01_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n1_p01_mmu_user",
                    cp_fn, code_ptr, 1, MMU_USER_IDX, 3, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n1_p01_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n0_p0_mmu_user",
                    cp_fn, code_ptr, 0, MMU_USER_IDX, 1, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n0_p0_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n1_p0_mmu_user",
                    cp_fn, code_ptr, 1, MMU_USER_IDX, 1, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n1_p0_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_user));
        }

        if (have_user && option_fast_itlb_ptw) {
            SET_SC_TABLE(mtcc_id, cp_check_ptw_user, (ADDR)code_ptr);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_user",
                    gen_latxs_cross_page_check_ptw, code_ptr, MMU_USER_IDX);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_user));

            if (tucp_fast_direct_check() || tucp_fast_fsht_check()) {
                SET_SC_TABLE(mtcc_id, tu_cp_check_n0_mmu_user_intcheck,
                             (ADDR)code_ptr);
                SET_SC_TABLE(mtcc_id, tu_cp_check_n0_mmu_user,
                             (ADDR)code_ptr + CP_INTCHECK_SIZE);
                LATXS_GEN_STATIC_CODES("tu_cp_check_n0_mmu_user",
                        gen_latxs_tu_cross_page_check, code_ptr, 0);

                SET_SC_TABLE(mtcc_id, tu_cp_check_n1_mmu_user_intcheck,
                             (ADDR)code_ptr);
                SET_SC_TABLE(mtcc_id, tu_cp_check_n1_mmu_user,
                             (ADDR)code_ptr + CP_INTCHECK_SIZE);
                LATXS_GEN_STATIC_CODES("tu_cp_check_n1_mmu_user",
                        gen_latxs_tu_cross_page_check, code_ptr, 1);

                SET_SC_TABLE(mtcc_id, tu_cp_check_ptr_mmu_user,
                             (ADDR)code_ptr);
                LATXS_GEN_STATIC_CODES("tu_cp_check_ptr_mmu_user",
                        gen_latxs_tu_cross_page_check_ptr, code_ptr);
            }

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n0_p01_mmu_user",
                    cp_fn, code_ptr, 0, MMU_USER_IDX, 3, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n0_p01_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n1_p01_mmu_user",
                    cp_fn, code_ptr, 1, MMU_USER_IDX, 3, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n1_p01_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n0_p0_mmu_user",
                    cp_fn, code_ptr, 0, MMU_USER_IDX, 1, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n0_p0_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n1_p0_mmu_user",
                    cp_fn, code_ptr, 1, MMU_USER_IDX, 1, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n1_p0_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_user));
        }

        if (have_knosmap) {
            SET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n0_p01_mmu_knosmap",
                    cp_fn, code_ptr, 0, MMU_KNOSMAP_IDX, 3, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n0_p01_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n1_p01_mmu_knosmap",
                    cp_fn, code_ptr, 1, MMU_KNOSMAP_IDX, 3, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n1_p01_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n0_p0_mmu_knosmap",
                    cp_fn, code_ptr, 0, MMU_KNOSMAP_IDX, 1, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n0_p0_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n1_p0_mmu_knosmap",
                    cp_fn, code_ptr, 1, MMU_KNOSMAP_IDX, 1, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n1_p0_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_knosmap));
        }

        if (have_knosmap && option_fast_itlb_ptw) {
            SET_SC_TABLE(mtcc_id, cp_check_ptw_knosmap, (ADDR)code_ptr);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_knosmap",
                    gen_latxs_cross_page_check_ptw, code_ptr, MMU_KNOSMAP_IDX);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n0_p01_mmu_knosmap",
                    cp_fn, code_ptr, 0, MMU_KNOSMAP_IDX, 3, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n0_p01_mmu_knosmap%p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n1_p01_mmu_knosmap",
                    cp_fn, code_ptr, 1, MMU_KNOSMAP_IDX, 3, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n1_p01_mmu_knosmap%p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n0_p0_mmu_knosmap",
                    cp_fn, code_ptr, 0, MMU_KNOSMAP_IDX, 1, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n0_p0_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n1_p0_mmu_knosmap",
                    cp_fn, code_ptr, 1, MMU_KNOSMAP_IDX, 1, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n1_p0_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_knosmap));
        }
    }
#endif

    if (intb_link_enable() && option_flaged_sht && tucp_fast_fsht_check() &&
        (!qemu_tcg_mtcc_enabled() || mtcc_id == 1)) {
        int index = 0;
        uint32_t flags = 0;

        FSHT_FOREACH_FLAGS(index, flags) {
            if ((flags & HF_CPL_MASK) == HF_CPL_MASK) {
                SET_SC_TABLE(mtcc_id, intb_fsht_lookup_tucpfast[index],
                             (ADDR)code_ptr);
                LATXS_GEN_STATIC_CODES("latxs_intb_fsht_lookup_tucpfast",
                        gen_latxs_intb_fsht_lookup_tucpfast, code_ptr, index);
            }
        }
    }

    latx_perfmap_flush();

#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    /* leave space for TBMini for the 1st TB */
    if (option_split_tb) {
        code_nr_all += qemu_icache_linesize / 4;
    }
#endif
    return code_nr_all;
}
