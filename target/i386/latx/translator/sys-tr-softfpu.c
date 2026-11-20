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
#include "flag-lbt.h"
#include "sys-excp.h"
#include "latx-sys-hamt-dmw.h"
#include "sys-aot.h"

#define LATXS_MOV_ADDR_TO_ARG(argx, src)                       \
    do {                                                       \
        if (latxs_ir1_addr_size(pir1) == 8) {                  \
            latxs_append_ir2_opnd2_(lisa_mov, &argx, &src);    \
        } else {                                               \
            latxs_append_ir2_opnd2_(lisa_mov32z, &argx, &src); \
        }                                                      \
    } while (0)

/**
* @brief imm_helper - The base interface that load immediate to gpr
*
* @param opnd2 - the dest opnd
* @param hi32
* @param lo32
*/
static void imm_helper(IR2_OPND opnd2, uint32_t hi32, uint32_t lo32)
{
    /*
     * We pay attention to sign extend beacause it is chance of
     * reduce insn. The exception is 12-bit and hi-12-bit unsigned,
     * we need a 'ori' or a 'lu52i.d' accordingly.
     */
    char sign_bitmap, all0_bitmap, allf_bitmap;
    char sign_extension_by_previous;
    char extension_by_previous_hi, extension_by_previous_lo;

    /*
     * We divided the imm64 into the following four parts and used
     * bitmap to represent the status of each part.
     *
     * | imm64      | bitmap |       |
     * | ---------- | ------ | ----- |
     * | imm[11:0]  | bit0   | part0 |
     * | imm[31:12] | bit1   | part1 |
     * | imm[51:32] | bit2   | part2 |
     * | imm[63:52] | bit3   | part3 |
     *
     */
    sign_bitmap = (((hi32 & 0x80000000) != 0) << 3) |
                  (((hi32 & 0x00080000) != 0) << 2) |
                  (((lo32 & 0x80000000) != 0) << 1) |
                  ((lo32 & 0x00000800) != 0);
    /* 1 means that this part equals 0 */
    all0_bitmap = (((hi32 & 0xfff00000) == 0) << 3) |
                  (((hi32 & 0x000fffff) == 0) << 2) |
                  (((lo32 & 0xfffff000) == 0) << 1) |
                  ((lo32 & 0x00000fff) == 0);
    /* 1 means that this part equals -1 */
    allf_bitmap = (((hi32 & 0xfff00000) == 0xfff00000) << 3) |
                  (((hi32 & 0x000fffff) == 0x000fffff) << 2) |
                  (((lo32 & 0xfffff000) == 0xfffff000) << 1) |
                  ((lo32 & 0x00000fff) == 0x00000fff);

    /*
     * Except part0, if the part is equal to 0 or -1, it may be a
     * sign extension of the previous part.
     *
     * bit0 equals 1 means part0 equals 0.
     */
    sign_extension_by_previous = (all0_bitmap ^ allf_bitmap) &
                                 (all0_bitmap ^ (sign_bitmap << 1));
    extension_by_previous_lo = sign_extension_by_previous & 0x3;
    extension_by_previous_hi = sign_extension_by_previous >> 2;

    /* the fast path */
    if (sign_extension_by_previous == 0x7) {
        /*
         * xxx 00000 00000 000
         * part0, part1 and part2 = 0, we just load part3
         */
        latxs_append_ir2_opnd2i(LISA_LU52I_D, &opnd2, &latxs_zero_ir2_opnd, hi32 >> 20);
        return;
    }

    /*
     * load lo32
     */
    switch (extension_by_previous_lo) {
    case 0x0:
        /*
         * lo32: 00000/xxxxx xxx
         */
        if ((all0_bitmap & 0x3) == 0x2) {
            /*
             * lo32: 00000 xxx
             * part1 = 0, we just load part0
             */
            latxs_append_ir2_opnd2i(LISA_ORI, &opnd2, &latxs_zero_ir2_opnd, lo32 & 0xfff);
        } else {
            /*
             * lo32: xxxxx xxx
             */
            latxs_append_ir2_opnd1i(LISA_LU12I_W, &opnd2, lo32 >> 12);
            latxs_append_ir2_opnd2i(LISA_ORI, &opnd2, &opnd2, lo32 & 0xfff);
        }
        break;
    case 0x1:
        /*
         * lo32: xxxxx 000
         */
        latxs_append_ir2_opnd1i(LISA_LU12I_W, &opnd2, lo32 >> 12);
        break;
    case 0x2:
        /*
         * lo32: 00000/fffff xxx
         * part0 != 0, part1 is sign extension of the part0
         */
        latxs_append_ir2_opnd2i(LISA_ADDI_W, &opnd2, &latxs_zero_ir2_opnd, lo32);
        break;
    case 0x3:
        /*
         * lo32: 00000 000
         */
        latxs_append_ir2_opnd3(LISA_OR, &opnd2, &latxs_zero_ir2_opnd, &latxs_zero_ir2_opnd);
        break;
    }

    /*
     * load hi32:
     * now, the lower 32 bits of the imm64 are loaded, and the
     * higher 32 bits of the opnd2 are signed extensions of the
     * lower.
     */
    switch (extension_by_previous_hi) {
    case 0x0:
        /*
         * hi32: xxx xxxxx
         */
        latxs_append_ir2_opnd1i(LISA_LU32I_D, &opnd2, hi32 & 0xfffff);
        latxs_append_ir2_opnd2i(LISA_LU52I_D, &opnd2, &opnd2, hi32 >> 20);
        break;
    case 0x1:
        /*
         * hi32: xxx 00000/fffff
         */
        latxs_append_ir2_opnd2i(LISA_LU52I_D, &opnd2, &opnd2, hi32 >> 20);
        break;
    case 0x2:
        /*
         * hi32: 000/fff xxxxx
         */
        latxs_append_ir2_opnd1i(LISA_LU32I_D, &opnd2, hi32 & 0xfffff);
        break;
    case 0x3:
        /*
         * hi32: 00000000/ffffffff
         */
        break;
    }
}

/**
* @brief li_d - macro instruction, semantic equivalent to li.d rd, {s,u}64
*
* @param opnd2 - the dest opnd
* @param value
*/
static void li_d(IR2_OPND opnd2, int64_t value)
{
    uint32_t hi32, lo32;

    lo32 = value & 0xffffffff;
    hi32 = value >> 32;
    imm_helper(opnd2, hi32, lo32);
}

/**
* @brief li_w - macro instruction, semantic equivalent to li.d rd, s32
*
* @param opnd2 - the dest opnd
* @param lo32
*/
static void li_w(IR2_OPND opnd2, uint32_t lo32)
{
    uint32_t hi32;

    hi32 = lo32 & 0x80000000 ? 0xffffffff : 0;
    imm_helper(opnd2, hi32, lo32);
}

/**
* @brief li_wu - macro instruction, semantic equivalent to li.d rd, u32
*
* @param opnd2 - the dest opnd
* @param lo32
*/
static void li_wu(IR2_OPND opnd2, uint32_t lo32)
{
    imm_helper(opnd2, 0, lo32);
}

static void la_fpu_push(void)
{
    IR2_OPND top_opnd = latxs_ra_alloc_itemp();

    latxs_append_ir2_opnd2i(LISA_LD_W, &top_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));
    latxs_append_ir2_opnd2i(LISA_ADDI_W, &top_opnd, &top_opnd, -1);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &top_opnd, &top_opnd, 2, 0);
    latxs_append_ir2_opnd2i(LISA_ST_W, &top_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));


    /* 2. mark st(0) as valid */
    int tag_offset = lsenv_offset_of_tag_word(lsenv);
    lsassert(tag_offset <= 0x7ff);
    latxs_append_ir2_opnd3(LISA_ADD_D, &top_opnd, &latxs_env_ir2_opnd, &top_opnd);
    latxs_append_ir2_opnd2i(LISA_ST_B, &latxs_zero_ir2_opnd, &top_opnd, tag_offset);


    latxs_ra_free_temp(&top_opnd);
}

static void la_fpu_pop(void)
{
    IR2_OPND top_opnd = latxs_ra_alloc_itemp();
    IR2_OPND temp = latxs_ra_alloc_itemp();
    IR2_OPND fptag = latxs_ra_alloc_itemp();


    latxs_append_ir2_opnd2i(LISA_LD_W, &top_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &top_opnd, &top_opnd, 2, 0);

    /* 1. mark st(0) as empty */
    int tag_offset = lsenv_offset_of_tag_word(lsenv);
    lsassert(tag_offset <= 0x7ff);

    li_d(temp, (uint64)(0x1ULL));
    latxs_append_ir2_opnd3(LISA_ADD_D, &fptag, &latxs_env_ir2_opnd, &top_opnd);
    latxs_append_ir2_opnd2i(LISA_ST_B, &temp, &fptag, tag_offset);

    latxs_append_ir2_opnd2i(LISA_ADDI_W, &temp, &top_opnd, 1);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &temp, &temp, 2, 0);
    latxs_append_ir2_opnd2i(LISA_ST_W, &temp, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));

    latxs_ra_free_temp(&temp);
    latxs_ra_free_temp(&fptag);
    latxs_ra_free_temp(&top_opnd);
}

static IR2_OPND get_stn_opnd_addr(int stn)
{
    IR2_OPND stn_opnd_addr = latxs_ra_alloc_itemp();

    latxs_append_ir2_opnd2i(LISA_LD_W, &stn_opnd_addr, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));
    if (0 != stn) {
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &stn_opnd_addr, &stn_opnd_addr, stn);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &stn_opnd_addr, &stn_opnd_addr, 2, 0);
    }

    latxs_append_ir2_opnd2i(LISA_SLLI_D, &stn_opnd_addr, &stn_opnd_addr, 4);
    latxs_append_ir2_opnd3(LISA_ADD_D, &stn_opnd_addr, &stn_opnd_addr, &latxs_env_ir2_opnd);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &stn_opnd_addr, &stn_opnd_addr, lsenv_offset_of_fpr(lsenv, 0));

    return stn_opnd_addr;
}

static void save_float80_to_stn(IR2_OPND stm_opnd, int stn)
{
    IR2_OPND stn_opnd_addr = get_stn_opnd_addr(stn);

    latxs_append_ir2_opnd2i(LISA_VST, &stm_opnd, &stn_opnd_addr, 0);

    latxs_ra_free_temp(&stn_opnd_addr);
}

static void get_float80_from_stn(IR2_OPND stm_opnd, int stn)
{
    IR2_OPND stn_opnd_addr = get_stn_opnd_addr(stn);

    latxs_append_ir2_opnd2i(LISA_VLD, &stm_opnd, &stn_opnd_addr, 0);

    latxs_ra_free_temp(&stn_opnd_addr);
}

static void fmov_STN_STM(int stn, int stm)
{
    if (stn == stm) {
        return;
    }
    IR2_OPND stm_opnd = latxs_ra_alloc_ftemp();

    get_float80_from_stn(stm_opnd, stm);
    save_float80_to_stn(stm_opnd, stn);

    latxs_ra_free_temp(&stm_opnd);
}

static void fmov_FT0_STN(int stn)
{
    IR2_OPND stm_opnd = latxs_ra_alloc_ftemp();

    get_float80_from_stn(stm_opnd, stn);
    latxs_append_ir2_opnd2i(LISA_VST, &stm_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_ft0(lsenv));

    latxs_ra_free_temp(&stm_opnd);
}

static void la_update_fp_status(IR2_OPND cw_opnd)
{
    IR2_OPND itemp = latxs_ra_alloc_itemp();
    IR2_OPND tmp_fcsr = latxs_ra_alloc_itemp();

    IR2_OPND label_float64_float80 = latxs_ir2_opnd_new_label();

    int fp_status_offset = lsenv_offset_of_fp_status(lsenv);
    int round_mode_offset = fp_status_offset +
                            offsetof(float_status, float_rounding_mode);
    int round_precision_offset = fp_status_offset +
                            offsetof(float_status, floatx80_rounding_precision);

    latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &tmp_fcsr, &latxs_fcsr_ir2_opnd);
    /* Rounding Control (11, 10)*/
    latxs_append_ir2_opnd2i(LISA_ORI, &itemp, &cw_opnd, 0);
    latxs_update_fcsr_rm(itemp, tmp_fcsr);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &cw_opnd, 11, 10);
    latxs_append_ir2_opnd2i(LISA_ST_B, &itemp, &latxs_env_ir2_opnd, round_mode_offset);

    /*
     *turn x86 pc to env pc
     *          x86      env
     *
     * 32       00       10
     * 64       10       01
     * 80       11       00
     *
     */

    /* Precision Control (9, 8)*/
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &cw_opnd, 9, 8);
    latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_float64_float80);
    li_wu(itemp, 1);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_float64_float80);
    latxs_append_ir2_opnd2i(LISA_XORI, &itemp, &itemp, 3);
    latxs_append_ir2_opnd2i(LISA_ST_B, &itemp, &latxs_env_ir2_opnd, round_precision_offset);

    latxs_ra_free_temp(&itemp);
    latxs_ra_free_temp(&tmp_fcsr);
}

bool latxs_translate_fld_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);

        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ftemp = latxs_ra_alloc_ftemp();

        if (ir1_opnd_is_fpr(opnd0)) {
            get_float80_from_stn(st0_opnd, ir1_opnd_base_reg_num(opnd0));
            la_fpu_push();
            save_float80_to_stn(st0_opnd, 0);

            latxs_ra_free_temp(&st0_opnd);
            latxs_ra_free_temp(&ftemp);
            return true;
        } else {
            if (opnd_size == 32 || opnd_size == 64) {
                IR2_OPND src_opnd = latxs_ra_alloc_ftemp();

                latxs_load_freg_from_ir1_2(&src_opnd, opnd0, true);
                latxs_append_ir2_opnd2(LISA_FCVT_UD_D, &ftemp, &src_opnd);
                latxs_append_ir2_opnd2(LISA_FCVT_LD_D, &st0_opnd, &src_opnd);
                latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &st0_opnd, &ftemp, 0x40);

                latxs_ra_free_temp(&src_opnd);
            }  else if (opnd_size == 80) {
                IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
                latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

                IR2_OPND itemp = latxs_ra_alloc_itemp();
                gen_ldst_softmmu_helper(LISA_LD_D, &itemp, &mem_opnd, 0, 1);
                latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &st0_opnd, &itemp);
                gen_ldst_softmmu_helper(LISA_LD_HU, &itemp, &mem_opnd, 8, 1);
                latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &ftemp, &itemp);
                latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &st0_opnd, &ftemp, 0x40);

                latxs_ra_free_temp(&itemp);
                latxs_ra_free_temp(&mem_opnd);
            }
        }

        la_fpu_push();
        save_float80_to_stn(st0_opnd, 0);

        latxs_ra_free_temp(&st0_opnd);
        latxs_ra_free_temp(&ftemp);
        return true;

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd);
        if (ir1_opnd_is_fpr(opnd)) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fpush,
                                             nofp_helper_cfg, HELPER_FPUSH);

            latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmov_ST0_STN,
                                             (ir1_opnd_base_reg_num(opnd) + 1) & 7,
                                             default_helper_cfg, HELPER_FMOV_ST0_STN);
            return true;
        } else if (opnd_size == 32) {
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
            latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                    &latxs_env_ir2_opnd);
            if (hamt_enabled()) {
                latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
            }
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
            latxs_tr_gen_call_to_helper((ADDR)helper_flds_ST0, HELPER_FLDS_ST0);
            latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
            latxs_ra_free_temp(&temp);
            return true;
        } else if (opnd_size == 64) {
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
            latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                    &latxs_env_ir2_opnd);
            if (hamt_enabled()) {
                latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
            }
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
            latxs_tr_gen_call_to_helper((ADDR)helper_fldl_ST0, HELPER_FLDL_ST0);
            latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
            latxs_ra_free_temp(&temp);
            return true;
        } else if (opnd_size == 80) {
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_convert_mem_opnd_no_offset(&temp, opnd);
            latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
            LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                    &latxs_env_ir2_opnd);
            if (hamt_enabled()) {
                latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
            }
            latxs_tr_gen_call_to_helper((ADDR)helper_fldt_ST0, HELPER_FLDT_ST0);
            latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
            latxs_ra_free_temp(&temp);
            return true;
        }
        lsassert(0);
        return false;
    }
}

bool latxs_translate_fild_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);

        IR2_OPND src_opnd = latxs_ra_alloc_itemp();
        IR2_OPND src_ftemp = latxs_ra_alloc_ftemp();
        latxs_load_ir1_to_ir2(&src_opnd, opnd0, EXMode_S);

        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ftemp_opnd = latxs_ra_alloc_ftemp();

        IR2_OPND label_exit = latxs_ir2_opnd_new_label();


        if (opnd_size == 64) {
            IR2_OPND itemp1 = latxs_ra_alloc_itemp();
            IR2_OPND itemp2 = latxs_ra_alloc_itemp();
            IR2_OPND label_hard = latxs_ir2_opnd_new_label();
            IR2_OPND label_positive = latxs_ir2_opnd_new_label();

            latxs_append_ir2_opnd2i(LISA_ORI, &itemp1, &src_opnd, 0);
            latxs_append_ir2_opnd3(LISA_BGE, &itemp1, &latxs_zero_ir2_opnd, &label_positive);
            latxs_append_ir2_opnd3(LISA_SUB_D, &itemp1, &latxs_zero_ir2_opnd, &src_opnd);

            /* src_opnd >=0 */
            latxs_append_ir2_opnd1(LISA_LABEL, &label_positive);
            latxs_append_ir2_opnd2(LISA_CLZ_D, &itemp1, &itemp1);
            li_d(itemp2, 12);
            latxs_append_ir2_opnd3(LISA_BGE, &itemp1, &itemp2, &label_hard);

            latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                    &latxs_env_ir2_opnd);
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &src_opnd);
            latxs_tr_gen_call_to_helper((ADDR)helper_fildll_ST0, HELPER_FILDLL_ST0);
            latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
            latxs_append_ir2_opnd1(LISA_B, &label_exit);

            latxs_append_ir2_opnd1(LISA_LABEL, &label_hard);
            latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &src_ftemp, &src_opnd);
            latxs_append_ir2_opnd2(LISA_FFINT_D_L, &src_ftemp, &src_ftemp);

            latxs_ra_free_temp(&itemp1);
            latxs_ra_free_temp(&itemp2);

        } else if (opnd_size == 32 || opnd_size == 16) {
            latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &src_ftemp, &src_opnd);
            latxs_append_ir2_opnd2(LISA_FFINT_D_W, &src_ftemp, &src_ftemp);
        } else {
            lsassert(0);
        }
        latxs_ra_free_temp(&src_opnd);

        latxs_append_ir2_opnd2(LISA_FCVT_LD_D, &st0_opnd, &src_ftemp);
        latxs_append_ir2_opnd2(LISA_FCVT_UD_D, &ftemp_opnd, &src_ftemp);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &st0_opnd, &ftemp_opnd, 0x40);

        la_fpu_push();
        save_float80_to_stn(st0_opnd, 0);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd);
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);

        latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
        if (opnd_size == 16 || opnd_size == 32) {
            latxs_tr_gen_call_to_helper((ADDR)helper_fildl_ST0, HELPER_FILDL_ST0);
        } else if (opnd_size == 64) {
            latxs_tr_gen_call_to_helper((ADDR)helper_fildll_ST0, HELPER_FILDLL_ST0);
        } else {
            lsassert(0);
        }
        latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
        latxs_ra_free_temp(&temp);
    }
    return true;
}

bool latxs_translate_fbld_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
    IR2_OPND temp = latxs_ra_alloc_itemp();
    latxs_convert_mem_opnd_no_offset(&temp, opnd);

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_tr_gen_call_to_helper((ADDR)helper_fbld_ST0, HELPER_FBLD_ST0);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
    } else {
        latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_tr_gen_call_to_helper((ADDR)helper_fbld_ST0, HELPER_FBLD_ST0);
        latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
    }
    latxs_ra_free_temp(&temp);
    return true;
}

bool latxs_translate_fst_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x800000) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);

        if (ir1_opnd_is_fpr(opnd0)) {
            int opnd0_base_num = ir1_opnd_base_reg_num(opnd0);

            fmov_STN_STM(opnd0_base_num, 0);
        } else {
            IR2_OPND itemp = latxs_ra_alloc_itemp();
            IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
            latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

            IR2_OPND low_opnd = latxs_ra_alloc_ftemp();
            IR2_OPND high_opnd = latxs_ra_alloc_ftemp();

            get_float80_from_stn(low_opnd, 0);
            latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &high_opnd, &low_opnd, 0x4);

            if (opnd_size == 80) {
                latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &high_opnd);
                gen_ldst_softmmu_helper(LISA_ST_H, &itemp, &mem_opnd, 8, 1);
                latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &low_opnd);
                gen_ldst_softmmu_helper(LISA_ST_D, &itemp, &mem_opnd, 0, 1);
                latxs_ra_free_temp(&low_opnd);
                latxs_ra_free_temp(&high_opnd);
                return true;
            }

            if (opnd_size == 32) {
                latxs_append_ir2_opnd3(LISA_FCVT_D_LD, &low_opnd, &low_opnd, &high_opnd);
                latxs_append_ir2_opnd2(LISA_FCVT_S_D, &low_opnd, &low_opnd);
                latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &low_opnd);
                gen_ldst_softmmu_helper(LISA_ST_W, &itemp, &mem_opnd, 0, 1);
            } else if (opnd_size == 64) {
                latxs_append_ir2_opnd3(LISA_FCVT_D_LD, &low_opnd, &low_opnd, &high_opnd);
                latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &low_opnd);
                gen_ldst_softmmu_helper(LISA_ST_D, &itemp, &mem_opnd, 0, 1);
            }
            latxs_ra_free_temp(&itemp);
            latxs_ra_free_temp(&mem_opnd);
        }
        return true;

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd);
        if (ir1_opnd_is_fpr(opnd)) {
            latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmov_STN_ST0,
                                             ir1_opnd_base_reg_num(opnd),
                                             default_helper_cfg,
                                             HELPER_FMOV_STN_ST0);
            return true;
        } else if (opnd_size == 32) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsts_ST0,
                                             default_helper_cfg, HELPER_FSTS_ST0);
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
            latxs_store_ir2_to_ir1(&temp, opnd);
            latxs_ra_free_temp(&temp);
            return true;
        } else if (opnd_size == 64) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fstl_ST0,
                                             default_helper_cfg, HELPER_FSTL_ST0);
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
            latxs_store_ir2_to_ir1(&temp, opnd);
            latxs_ra_free_temp(&temp);
            return true;
        }
        lsassert(0);
        return false;
    }
}

#define FLOAT80_EXPONENT_MAX 0x3fff
#define FLOAT64_EXPONENT_MAX 51
#define FLOAT32_EXPONENT_MAX 23

bool latxs_translate_fist_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x200000) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);
        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

        IR2_OPND itemp = latxs_ra_alloc_itemp();
        IR2_OPND itemp2 = latxs_ra_alloc_itemp();

        IR2_OPND label_exit = latxs_ir2_opnd_new_label();
        IR2_OPND label_softfpu = latxs_ir2_opnd_new_label();

        int round_mode_offset = lsenv_offset_of_fp_status(lsenv) +
                                offsetof(float_status, float_rounding_mode);

        latxs_append_ir2_opnd2i(LISA_LD_BU, &itemp, &latxs_env_ir2_opnd, round_mode_offset);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp2, &itemp, 2, 2);
        latxs_append_ir2_opnd3(LISA_BNE, &itemp2, &latxs_zero_ir2_opnd, &label_softfpu);

        IR2_OPND fpr_st0_low_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND fpr_st0_high_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND st0_opnd = fpr_st0_low_opnd;

        get_float80_from_stn(fpr_st0_low_opnd, 0);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &fpr_st0_high_opnd, &fpr_st0_low_opnd, 0x4);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &fpr_st0_high_opnd);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 14, 0);
        li_d(itemp2, FLOAT80_EXPONENT_MAX + FLOAT32_EXPONENT_MAX + 1);
        latxs_append_ir2_opnd3(LISA_BGE, &itemp, &itemp2, &label_softfpu);

        if (opnd_size == 16) {
            latxs_append_ir2_opnd3(LISA_FCVT_D_LD, &st0_opnd, &fpr_st0_low_opnd, &fpr_st0_high_opnd);
            latxs_append_ir2_opnd2(LISA_FTINT_W_D, &st0_opnd, &st0_opnd);
            latxs_append_ir2_opnd2(LISA_MOVFR2GR_S, &itemp, &st0_opnd);
            gen_ldst_softmmu_helper(LISA_ST_H, &itemp, &mem_opnd, 0, 1);

        } else if (opnd_size == 32) {
            latxs_append_ir2_opnd3(LISA_FCVT_D_LD, &st0_opnd, &fpr_st0_low_opnd, &fpr_st0_high_opnd);
            latxs_append_ir2_opnd2(LISA_FTINT_W_D, &st0_opnd, &st0_opnd);
            latxs_append_ir2_opnd2(LISA_MOVFR2GR_S, &itemp, &st0_opnd);
            gen_ldst_softmmu_helper(LISA_ST_W, &itemp, &mem_opnd, 0, 1);

        }
        latxs_append_ir2_opnd1(LISA_B, &label_exit);
        latxs_ra_free_temp(&itemp);
        latxs_ra_free_temp(&itemp2);
        latxs_ra_free_temp(&mem_opnd);


        latxs_append_ir2_opnd1(LISA_LABEL, &label_softfpu);
        if (opnd_size == 16) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fist_ST0,
                                             nofp_helper_cfg, HELPER_FIST_ST0);
        } else if (opnd_size == 32) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fistl_ST0,
                                             nofp_helper_cfg, HELPER_FISTL_ST0);
        } else {
            lsassert(0);
        }
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
        latxs_store_ir2_to_ir1(&temp, opnd0);
        latxs_ra_free_temp(&temp);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    } else {
        IR1_OPND *dst = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(dst);
        if (opnd_size == 16) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fist_ST0,
                                             default_helper_cfg, HELPER_FIST_ST0);
        } else if (opnd_size == 32) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fistl_ST0,
                                             default_helper_cfg, HELPER_FISTL_ST0);
        } else if (opnd_size == 64) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fistll_ST0,
                                             default_helper_cfg, HELPER_FISTLL_ST0);
        } else {
            lsassert(0);
        }
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
        latxs_store_ir2_to_ir1(&temp, dst);
        latxs_ra_free_temp(&temp);
    }
    return true;
}

bool latxs_translate_fstp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x800000) {
        latxs_translate_fst_softfpu(pir1);
        la_fpu_pop();
        return true;
    } else {
        bool is_fstp_3opnd = ir1_opnd_num(pir1) == 3;

        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd);
        if (ir1_opnd_is_fpr(opnd)) {
            if (is_fstp_3opnd) {
                opnd = ir1_get_opnd(pir1, 1);
            }
            latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmov_STN_ST0,
                                             ir1_opnd_base_reg_num(opnd),
                                             default_helper_cfg,
                                             HELPER_FMOV_STN_ST0);
            la_fpu_pop();
            return true;
        } else if (opnd_size == 32) {
            lsassertm(!is_fstp_3opnd, "undocumented fstp");
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsts_ST0,
                                             default_helper_cfg, HELPER_FSTS_ST0);
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
            latxs_store_ir2_to_ir1(&temp, opnd);
            latxs_ra_free_temp(&temp);
            la_fpu_pop();
            return true;
        } else if (opnd_size == 64) {
            lsassertm(!is_fstp_3opnd, "undocumented fstp");
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fstl_ST0,
                                             default_helper_cfg, HELPER_FSTL_ST0);
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
            latxs_store_ir2_to_ir1(&temp, opnd);
            latxs_ra_free_temp(&temp);
            la_fpu_pop();
            return true;
        } else if (opnd_size == 80) {
            lsassertm(!is_fstp_3opnd, "undocumented fstp");
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_convert_mem_opnd_no_offset(&temp, opnd);
            latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);

            LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                    &latxs_env_ir2_opnd);
            if (hamt_enabled()) {
                latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
            }
            latxs_tr_gen_call_to_helper((ADDR)helper_fstt_ST0, HELPER_FSTT_ST0);
            latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
            la_fpu_pop();
            latxs_ra_free_temp(&temp);
            return true;
        }
        lsassert(0);
        return false;
    }
}

bool latxs_translate_fisttp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        IR1_OPND *dst = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(dst);

        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }

        if (opnd_size == 16) {
            latxs_tr_gen_call_to_helper((ADDR)helper_fistt_ST0, HELPER_FISTT_ST0);
        } else if (opnd_size == 32) {
            latxs_tr_gen_call_to_helper((ADDR)helper_fisttl_ST0, HELPER_FISTTL_ST0);
        } else if (opnd_size == 64) {
            latxs_tr_gen_call_to_helper((ADDR)helper_fisttll_ST0, HELPER_FISTTLL_ST0);
        } else {
            lsassert(0);
        }

        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
        latxs_store_ir2_to_ir1(&temp, dst);
        latxs_ra_free_temp(&temp);

    } else {
        IR1_OPND *dst = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(dst);

        if (opnd_size == 16) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fistt_ST0,
                                             default_helper_cfg, HELPER_FISTT_ST0);
        } else if (opnd_size == 32) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fisttl_ST0,
                                             default_helper_cfg, HELPER_FISTTL_ST0);
        } else if (opnd_size == 64) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fisttll_ST0,
                                             default_helper_cfg, HELPER_FISTTLL_ST0);
        } else {
            lsassert(0);
        }

        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
        latxs_store_ir2_to_ir1(&temp, dst);
        latxs_ra_free_temp(&temp);
    }
    la_fpu_pop();

    return true;
}

bool latxs_translate_fistp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x400000) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);

        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

        IR2_OPND itemp = latxs_ra_alloc_itemp();
        IR2_OPND itemp2 = latxs_ra_alloc_itemp();

        IR2_OPND label_exit = latxs_ir2_opnd_new_label();
        IR2_OPND label_softfpu = latxs_ir2_opnd_new_label();

        int round_mode_offset = lsenv_offset_of_fp_status(lsenv) +
                                offsetof(float_status, float_rounding_mode);

        latxs_append_ir2_opnd2i(LISA_LD_BU, &itemp, &latxs_env_ir2_opnd, round_mode_offset);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp2, &itemp, 2, 2);
        latxs_append_ir2_opnd3(LISA_BNE, &itemp2, &latxs_zero_ir2_opnd, &label_softfpu);

        IR2_OPND fpr_st0_low_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND fpr_st0_high_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND st0_opnd = fpr_st0_low_opnd;

        get_float80_from_stn(fpr_st0_low_opnd, 0);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &fpr_st0_high_opnd, &fpr_st0_low_opnd, 0x4);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &fpr_st0_high_opnd);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 14, 0);

        if (opnd_size == 16) {
            li_d(itemp2, FLOAT80_EXPONENT_MAX + FLOAT32_EXPONENT_MAX + 1);
            latxs_append_ir2_opnd3(LISA_BGE, &itemp, &itemp2, &label_softfpu);

            latxs_append_ir2_opnd3(LISA_FCVT_D_LD, &st0_opnd, &fpr_st0_low_opnd, &fpr_st0_high_opnd);
            latxs_append_ir2_opnd2(LISA_FTINT_W_D, &st0_opnd, &st0_opnd);
            latxs_append_ir2_opnd2(LISA_MOVFR2GR_S, &itemp, &st0_opnd);
            gen_ldst_softmmu_helper(LISA_ST_H, &itemp, &mem_opnd, 0, 1);

        } else if (opnd_size == 32) {
            li_d(itemp2, FLOAT80_EXPONENT_MAX + FLOAT32_EXPONENT_MAX + 1);
            latxs_append_ir2_opnd3(LISA_BGE, &itemp, &itemp2, &label_softfpu);

            latxs_append_ir2_opnd3(LISA_FCVT_D_LD, &st0_opnd, &fpr_st0_low_opnd, &fpr_st0_high_opnd);
            latxs_append_ir2_opnd2(LISA_FTINT_W_D, &st0_opnd, &st0_opnd);
            latxs_append_ir2_opnd2(LISA_MOVFR2GR_S, &itemp, &st0_opnd);
            gen_ldst_softmmu_helper(LISA_ST_W, &itemp, &mem_opnd, 0, 1);

        } else if (opnd_size == 64) {
            li_d(itemp2, FLOAT80_EXPONENT_MAX + FLOAT64_EXPONENT_MAX + 1);
            latxs_append_ir2_opnd3(LISA_BGE, &itemp, &itemp2, &label_softfpu);

            latxs_append_ir2_opnd3(LISA_FCVT_D_LD, &st0_opnd, &fpr_st0_low_opnd, &fpr_st0_high_opnd);
            latxs_append_ir2_opnd2(LISA_FTINT_L_D, &st0_opnd, &st0_opnd);
            latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &st0_opnd);
            gen_ldst_softmmu_helper(LISA_ST_D, &itemp, &mem_opnd, 0, 1);
        }

        la_fpu_pop();
        latxs_append_ir2_opnd1(LISA_B, &label_exit);
        latxs_ra_free_temp(&itemp);
        latxs_ra_free_temp(&itemp2);
        latxs_ra_free_temp(&mem_opnd);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_softfpu);
        if (opnd_size == 16) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fist_ST0,
                                             nofp_helper_cfg, HELPER_FIST_ST0);
        } else if (opnd_size == 32) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fistl_ST0,
                                             nofp_helper_cfg, HELPER_FISTL_ST0);
        } else if (opnd_size == 64) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fistll_ST0,
                                             nofp_helper_cfg, HELPER_FISTLL_ST0);
        } else {
            lsassert(0);
        }
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
        latxs_store_ir2_to_ir1(&temp, opnd0);
        latxs_ra_free_temp(&temp);
        la_fpu_pop();

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    } else {
        IR1_OPND *dst = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(dst);
        if (opnd_size == 16) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fist_ST0,
                                             default_helper_cfg, HELPER_FIST_ST0);
        } else if (opnd_size == 32) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fistl_ST0,
                                             default_helper_cfg, HELPER_FISTL_ST0);
        } else if (opnd_size == 64) {
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fistll_ST0,
                                             default_helper_cfg, HELPER_FISTLL_ST0);
        } else {
            lsassert(0);
        }
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
        latxs_store_ir2_to_ir1(&temp, dst);
        latxs_ra_free_temp(&temp);
        la_fpu_pop();
    }
    return true;
}

bool latxs_translate_fbstp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
    IR2_OPND temp = latxs_ra_alloc_itemp();

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);

        latxs_convert_mem_opnd_no_offset(&temp, opnd);

        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_tr_gen_call_to_helper((ADDR)helper_fbst_ST0, HELPER_FBST_ST0);

        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        la_fpu_pop();

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        IR2_OPND temp = latxs_ra_alloc_itemp();

        latxs_convert_mem_opnd_no_offset(&temp, opnd);

        latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_tr_gen_call_to_helper((ADDR)helper_fbst_ST0, HELPER_FBST_ST0);
        latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);

        la_fpu_pop();
    }

    latxs_ra_free_temp(&temp);
    return true;
}

bool latxs_translate_fxch_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    IR1_OPND *src = ir1_get_opnd(pir1, 0);

    if (option_soft_fpu_fast) {
        int stn = ir1_opnd_base_reg_num(src);
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND stn_opnd = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);
        get_float80_from_stn(stn_opnd, stn);

        save_float80_to_stn(st0_opnd, stn);
        save_float80_to_stn(stn_opnd, 0);

    } else {
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fxchg_ST0_STN,
                                         ir1_opnd_base_reg_num(src),
                                         default_helper_cfg, HELPER_FXCHG_ST0_STN);
    }
    return true;
}

bool latxs_translate_fcmovb_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1) ||
        latxs_tr_gen_excp_check(pir1)) {
        return true;
    }

    IR2_OPND judge_result = latxs_ra_alloc_itemp();
    latxs_get_eflag_condition(&judge_result, pir1);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &judge_result, &latxs_zero_ir2_opnd,
                           &label_exit);

    IR1_OPND *opnd = ir1_get_opnd(pir1, 1);
    int reg_num = ir1_opnd_base_reg_num(opnd);
    if (option_soft_fpu_fast) {
        fmov_STN_STM(0, reg_num);
    } else {
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmov_ST0_STN, reg_num,
                                         default_helper_cfg, HELPER_FMOV_ST0_STN);
    }

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    latxs_ra_free_temp(&judge_result);
    return true;
}

bool latxs_translate_fcmove_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1) ||
        latxs_tr_gen_excp_check(pir1)) {
        return true;
    }

    IR2_OPND judge_result = latxs_ra_alloc_itemp();
    latxs_get_eflag_condition(&judge_result, pir1);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &judge_result, &latxs_zero_ir2_opnd,
                           &label_exit);

    IR1_OPND *opnd = ir1_get_opnd(pir1, 1);
    int reg_num = ir1_opnd_base_reg_num(opnd);
    if (option_soft_fpu_fast) {
        fmov_STN_STM(0, reg_num);
    } else {
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmov_ST0_STN, reg_num,
                                         default_helper_cfg, HELPER_FMOV_ST0_STN);
    }

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    latxs_ra_free_temp(&judge_result);
    return true;
}

bool latxs_translate_fcmovbe_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1) ||
        latxs_tr_gen_excp_check(pir1)) {
        return true;
    }

    IR2_OPND judge_result = latxs_ra_alloc_itemp();
    latxs_get_eflag_condition(&judge_result, pir1);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &judge_result, &latxs_zero_ir2_opnd,
                           &label_exit);

    IR1_OPND *opnd = ir1_get_opnd(pir1, 1);
    int reg_num = ir1_opnd_base_reg_num(opnd);
    if (option_soft_fpu_fast) {
        fmov_STN_STM(0, reg_num);
    } else {
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmov_ST0_STN, reg_num,
                                         default_helper_cfg, HELPER_FMOV_ST0_STN);
    }

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    latxs_ra_free_temp(&judge_result);
    return true;
}

bool latxs_translate_fcmovu_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1) ||
        latxs_tr_gen_excp_check(pir1)) {
        return true;
    }

    IR2_OPND judge_result = latxs_ra_alloc_itemp();

    latxs_get_eflag_condition(&judge_result, pir1);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &judge_result, &latxs_zero_ir2_opnd,
                           &label_exit);

    IR1_OPND *opnd = ir1_get_opnd(pir1, 1);
    int reg_num = ir1_opnd_base_reg_num(opnd);
    if (option_soft_fpu_fast) {
        fmov_STN_STM(0, reg_num);
    } else {
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmov_ST0_STN, reg_num,
                                         default_helper_cfg, HELPER_FMOV_ST0_STN);
    }

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    latxs_ra_free_temp(&judge_result);
    return true;
}

bool latxs_translate_fcmovnb_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1) ||
        latxs_tr_gen_excp_check(pir1)) {
        return true;
    }

    IR2_OPND judge_result = latxs_ra_alloc_itemp();

    latxs_get_eflag_condition(&judge_result, pir1);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &judge_result, &latxs_zero_ir2_opnd,
                           &label_exit);

    IR1_OPND *opnd = ir1_get_opnd(pir1, 1);
    int reg_num = ir1_opnd_base_reg_num(opnd);
    if (option_soft_fpu_fast) {
        fmov_STN_STM(0, reg_num);
    } else {
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmov_ST0_STN, reg_num,
                                         default_helper_cfg, HELPER_FMOV_ST0_STN);
    }

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    latxs_ra_free_temp(&judge_result);
    return true;
}

bool latxs_translate_fcmovne_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1) ||
        latxs_tr_gen_excp_check(pir1)) {
        return true;
    }

    IR2_OPND judge_result = latxs_ra_alloc_itemp();

    latxs_get_eflag_condition(&judge_result, pir1);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &judge_result, &latxs_zero_ir2_opnd,
                           &label_exit);

    IR1_OPND *opnd = ir1_get_opnd(pir1, 1);
    int reg_num = ir1_opnd_base_reg_num(opnd);
    if (option_soft_fpu_fast) {
        fmov_STN_STM(0, reg_num);
    } else {
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmov_ST0_STN, reg_num,
                                         default_helper_cfg, HELPER_FMOV_ST0_STN);
    }

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    latxs_ra_free_temp(&judge_result);
    return true;
}

bool latxs_translate_fcmovnbe_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1) ||
        latxs_tr_gen_excp_check(pir1)) {
        return true;
    }

    IR2_OPND judge_result = latxs_ra_alloc_itemp();
    latxs_get_eflag_condition(&judge_result, pir1);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &judge_result, &latxs_zero_ir2_opnd,
                           &label_exit);

    IR1_OPND *opnd = ir1_get_opnd(pir1, 1);
    int reg_num = ir1_opnd_base_reg_num(opnd);
    if (option_soft_fpu_fast) {
        fmov_STN_STM(0, reg_num);
    } else {
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmov_ST0_STN, reg_num,
                                         default_helper_cfg, HELPER_FMOV_ST0_STN);
    }

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    latxs_ra_free_temp(&judge_result);
    return true;
}

bool latxs_translate_fcmovnu_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1) ||
        latxs_tr_gen_excp_check(pir1)) {
        return true;
    }

    IR2_OPND judge_result = latxs_ra_alloc_itemp();
    latxs_get_eflag_condition(&judge_result, pir1);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &judge_result, &latxs_zero_ir2_opnd,
                           &label_exit);

    IR1_OPND *opnd = ir1_get_opnd(pir1, 1);
    int reg_num = ir1_opnd_base_reg_num(opnd);
    if (option_soft_fpu_fast) {
        fmov_STN_STM(0, reg_num);
    } else {
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmov_ST0_STN, reg_num,
                                         default_helper_cfg, HELPER_FMOV_ST0_STN);
    }

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    latxs_ra_free_temp(&judge_result);
    return true;
}

bool latxs_translate_fldz_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();

        la_fpu_push();
        latxs_append_ir2_opnd2i(LISA_VANDI_B, &st0_opnd, &st0_opnd, 0);
        save_float80_to_stn(st0_opnd, 0);

    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fpush, nofp_helper_cfg, HELPER_FPUSH);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fldz_ST0, default_helper_cfg,
                HELPER_FLDZ_ST0);
    }
    return true;
}

bool latxs_translate_fld1_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ftemp = latxs_ra_alloc_ftemp();
        IR2_OPND itemp = latxs_ra_alloc_itemp();

        la_fpu_push();
        li_d(itemp, 0x8000000000000000LL);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &st0_opnd, &itemp);
        li_wu(itemp, 0x3fff);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &ftemp, &itemp);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &st0_opnd, &ftemp, 0x40);

        save_float80_to_stn(st0_opnd, 0);
        latxs_ra_free_temp(&itemp);

    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fpush, nofp_helper_cfg, HELPER_FPUSH);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fld1_ST0, default_helper_cfg,
                HELPER_FLD1_ST0);
    }
    return true;
}

bool latxs_translate_fldl2e_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ftemp = latxs_ra_alloc_ftemp();
        IR2_OPND itemp = latxs_ra_alloc_itemp();
        IR2_OPND cw_opnd = latxs_ra_alloc_itemp();

        IR2_OPND label_default = latxs_ir2_opnd_new_label();
        IR2_OPND label_exit = latxs_ir2_opnd_new_label();

        la_fpu_push();
        li_wu(itemp, 0x3fff);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &ftemp, &itemp);
        latxs_append_ir2_opnd2i(LISA_LD_HU, &cw_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_control_word(lsenv));
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &cw_opnd, &cw_opnd, 10, 10);

        latxs_append_ir2_opnd3(LISA_BEQ, &cw_opnd, &latxs_zero_ir2_opnd, &label_default);
        li_d(itemp, 0xb8aa3b295c17f0bbLL);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_default);
        li_d(itemp, 0xb8aa3b295c17f0bcLL);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &st0_opnd, &itemp);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &st0_opnd, &ftemp, 0x40);
        save_float80_to_stn(st0_opnd, 0);

        latxs_ra_free_temp(&itemp);
        latxs_ra_free_temp(&cw_opnd);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fpush, nofp_helper_cfg, HELPER_FPUSH);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fldl2e_ST0,
                                         default_helper_cfg, HELPER_FLDL2E_ST0);
    }
    return true;
}

bool latxs_translate_fldl2t_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ftemp = latxs_ra_alloc_ftemp();
        IR2_OPND itemp = latxs_ra_alloc_itemp();
        IR2_OPND cw_opnd = latxs_ra_alloc_itemp();

        IR2_OPND label_default = latxs_ir2_opnd_new_label();
        IR2_OPND label_exit = latxs_ir2_opnd_new_label();

        la_fpu_push();
        li_wu(itemp, 0x4000);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &ftemp, &itemp);
        latxs_append_ir2_opnd2i(LISA_LD_HU, &cw_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_control_word(lsenv));
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &cw_opnd, &cw_opnd, 11, 11);

        latxs_append_ir2_opnd3(LISA_BEQ, &cw_opnd, &latxs_zero_ir2_opnd, &label_default);
        li_d(itemp, 0xd49a784bcd1b8affLL);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_default);
        li_d(itemp, 0xd49a784bcd1b8afeLL);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &st0_opnd, &itemp);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &st0_opnd, &ftemp, 0x40);
        save_float80_to_stn(st0_opnd, 0);

        latxs_ra_free_temp(&itemp);
        latxs_ra_free_temp(&cw_opnd);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fpush, nofp_helper_cfg, HELPER_FPUSH);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fldl2t_ST0,
                                         default_helper_cfg, HELPER_FLDL2T_ST0);
    }
    return true;
}

bool latxs_translate_fldlg2_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ftemp = latxs_ra_alloc_ftemp();
        IR2_OPND itemp = latxs_ra_alloc_itemp();
        IR2_OPND cw_opnd = latxs_ra_alloc_itemp();

        IR2_OPND label_default = latxs_ir2_opnd_new_label();
        IR2_OPND label_exit = latxs_ir2_opnd_new_label();

        la_fpu_push();
        li_wu(itemp, 0x3ffd);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &ftemp, &itemp);
        latxs_append_ir2_opnd2i(LISA_LD_HU, &cw_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_control_word(lsenv));
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &cw_opnd, &cw_opnd, 10, 10);

        latxs_append_ir2_opnd3(LISA_BEQ, &cw_opnd, &latxs_zero_ir2_opnd, &label_default);
        li_d(itemp, 0x9a209a84fbcff798LL);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_default);
        li_d(itemp, 0x9a209a84fbcff799LL);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &st0_opnd, &itemp);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &st0_opnd, &ftemp, 0x40);
        save_float80_to_stn(st0_opnd, 0);

        latxs_ra_free_temp(&itemp);
        latxs_ra_free_temp(&cw_opnd);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fpush, nofp_helper_cfg, HELPER_FPUSH);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fldlg2_ST0,
                                         default_helper_cfg, HELPER_FLDLG2_ST0);
    }
    return true;
}

bool latxs_translate_fldln2_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ftemp = latxs_ra_alloc_ftemp();
        IR2_OPND itemp = latxs_ra_alloc_itemp();
        IR2_OPND cw_opnd = latxs_ra_alloc_itemp();

        IR2_OPND label_default = latxs_ir2_opnd_new_label();
        IR2_OPND label_exit = latxs_ir2_opnd_new_label();

        la_fpu_push();
        li_wu(itemp, 0x3ffe);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &ftemp, &itemp);
        latxs_append_ir2_opnd2i(LISA_LD_HU, &cw_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_control_word(lsenv));
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &cw_opnd, &cw_opnd, 10, 10);

        latxs_append_ir2_opnd3(LISA_BEQ, &cw_opnd, &latxs_zero_ir2_opnd, &label_default);
        li_d(itemp, 0xb17217f7d1cf79abLL);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_default);
        li_d(itemp, 0xb17217f7d1cf79acLL);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &st0_opnd, &itemp);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &st0_opnd, &ftemp, 0x40);
        save_float80_to_stn(st0_opnd, 0);

        latxs_ra_free_temp(&itemp);
        latxs_ra_free_temp(&cw_opnd);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fpush, nofp_helper_cfg, HELPER_FPUSH);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fldln2_ST0,
                                         default_helper_cfg, HELPER_FLDLN2_ST0);
    }
    return true;
}

bool latxs_translate_fldpi_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ftemp = latxs_ra_alloc_ftemp();
        IR2_OPND itemp = latxs_ra_alloc_itemp();
        IR2_OPND cw_opnd = latxs_ra_alloc_itemp();

        IR2_OPND label_default = latxs_ir2_opnd_new_label();
        IR2_OPND label_exit = latxs_ir2_opnd_new_label();

        la_fpu_push();
        li_wu(itemp, 0x4000);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &ftemp, &itemp);
        latxs_append_ir2_opnd2i(LISA_LD_HU, &cw_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_control_word(lsenv));
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &cw_opnd, &cw_opnd, 10, 10);

        latxs_append_ir2_opnd3(LISA_BEQ, &cw_opnd, &latxs_zero_ir2_opnd, &label_default);
        li_d(itemp, 0xc90fdaa22168c234LL);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_default);
        li_d(itemp, 0xc90fdaa22168c235LL);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &st0_opnd, &itemp);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &st0_opnd, &ftemp, 0x40);
        save_float80_to_stn(st0_opnd, 0);

        latxs_ra_free_temp(&itemp);
        latxs_ra_free_temp(&cw_opnd);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fpush, nofp_helper_cfg, HELPER_FPUSH);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fldpi_ST0,
                                         default_helper_cfg, HELPER_FLDPI_ST0);
    }
    return true;
}

static void la_calcute_float64(IR1_INST *pir1, IR2_OPND float80_dest,
                               IR2_OPND src_opnd, bool src_is_float80)
{
    IR2_OPND ftemp = latxs_ra_alloc_ftemp();

    latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &float80_dest, 0x4);
    latxs_append_ir2_opnd3(LISA_FCVT_D_LD, &float80_dest, &float80_dest, &ftemp);

    if (src_is_float80) {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &src_opnd, 0x4);
        latxs_append_ir2_opnd3(LISA_FCVT_D_LD, &src_opnd, &src_opnd, &ftemp);
    }

    switch (ir1_opcode(pir1)) {
    case X86_INS_FADD:
    case X86_INS_FADDP:
    case X86_INS_FIADD:
        latxs_append_ir2_opnd3(LISA_FADD_D, &float80_dest, &float80_dest, &src_opnd);
        break;
    case X86_INS_FSUB:
    case X86_INS_FSUBP:
    case X86_INS_FISUB:
        latxs_append_ir2_opnd3(LISA_FSUB_D, &float80_dest, &float80_dest, &src_opnd);
        break;
    case X86_INS_FSUBR:
    case X86_INS_FSUBRP:
    case X86_INS_FISUBR:
        latxs_append_ir2_opnd3(LISA_FSUB_D, &float80_dest, &src_opnd, &float80_dest);
        break;
    case X86_INS_FMUL:
    case X86_INS_FMULP:
    case X86_INS_FIMUL:
        latxs_append_ir2_opnd3(LISA_FMUL_D, &float80_dest, &float80_dest, &src_opnd);
        break;
    case X86_INS_FDIV:
    case X86_INS_FDIVP:
    case X86_INS_FIDIV:
        latxs_append_ir2_opnd3(LISA_FDIV_D, &float80_dest, &float80_dest, &src_opnd);
        break;
    case X86_INS_FDIVR:
    case X86_INS_FDIVRP:
    case X86_INS_FIDIVR:
        latxs_append_ir2_opnd3(LISA_FDIV_D, &float80_dest, &src_opnd, &float80_dest);
        break;
    case X86_INS_FSQRT:
        latxs_append_ir2_opnd2(LISA_FSQRT_D, &float80_dest, &src_opnd);
        break;
    case X86_INS_FRNDINT:
        latxs_append_ir2_opnd2(LISA_FRINT_D, &float80_dest, &src_opnd);
        break;
    case X86_INS_FSCALE:
        latxs_append_ir2_opnd2(LISA_FTINTRZ_L_D, &ftemp, &src_opnd);
        latxs_append_ir2_opnd3(LISA_FSCALEB_D, &float80_dest, &float80_dest, &ftemp);
        break;
    default:
        lsassert(0);
        break;
    }

    latxs_append_ir2_opnd2(LISA_FCVT_UD_D, &ftemp, &float80_dest);
    latxs_append_ir2_opnd2(LISA_FCVT_LD_D, &float80_dest, &float80_dest);
    latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &float80_dest, &ftemp, 0x40);

    latxs_ra_free_temp(&ftemp);

}

static void gen_float64_ST0_MEM(IR1_INST *pir1, IR2_OPND mem_opnd,
                                int opnd_size, bool is_integer)
{
    IR2_OPND itemp = latxs_ra_alloc_itemp();
    IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
    IR2_OPND src_opnd = latxs_ra_alloc_ftemp();

    if (opnd_size == 64) {
        gen_ldst_softmmu_helper(LISA_LD_D, &itemp, &mem_opnd, 0, 1);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &src_opnd, &itemp);
        if (is_integer) {
            latxs_append_ir2_opnd2(LISA_FFINT_D_L, &src_opnd, &src_opnd);
        }
    } else if (opnd_size == 32) {
        gen_ldst_softmmu_helper(LISA_LD_W, &itemp, &mem_opnd, 0, 1);
        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &src_opnd, &itemp);
        if (is_integer) {
            latxs_append_ir2_opnd2(LISA_FFINT_D_W, &src_opnd, &src_opnd);
        } else {
            latxs_append_ir2_opnd2(LISA_FCVT_D_S, &src_opnd, &src_opnd);
        }
    } else if (opnd_size == 16) {
        gen_ldst_softmmu_helper(LISA_LD_H, &itemp, &mem_opnd, 0, 1);
        if (is_integer) {
            latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &src_opnd, &itemp);
            latxs_append_ir2_opnd2(LISA_FFINT_D_W, &src_opnd, &src_opnd);
        } else {
            lsassertm(!is_integer, "convert 16-bit floating point?\n");
        }
    } else {
        lsassert(0);
    }
    latxs_ra_free_temp(&itemp);

    get_float80_from_stn(st0_opnd, 0);
    la_calcute_float64(pir1, st0_opnd, src_opnd, false);
    save_float80_to_stn(st0_opnd, 0);

    latxs_ra_free_temp(&st0_opnd);
    latxs_ra_free_temp(&src_opnd);
}

static void gen_float64_STN_ST0(IR1_INST *pir1, int opnd_num, int stn)
{
    IR2_OPND stn_opnd = latxs_ra_alloc_ftemp();
    IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();

    get_float80_from_stn(stn_opnd, stn);
    get_float80_from_stn(st0_opnd, 0);
    if (opnd_num == 1) {
        la_calcute_float64(pir1, st0_opnd, stn_opnd, true);
        save_float80_to_stn(st0_opnd, 0);
    } else if (opnd_num == 2) {
        la_calcute_float64(pir1, stn_opnd, st0_opnd, true);
        save_float80_to_stn(stn_opnd, stn);
    }

    latxs_ra_free_temp(&stn_opnd);
    latxs_ra_free_temp(&st0_opnd);
}

static void gen_float64_STN_ST0_pop(IR1_INST *pir1, int stn)
{
    IR2_OPND stn_opnd = latxs_ra_alloc_ftemp();
    IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();

    get_float80_from_stn(stn_opnd, stn);
    get_float80_from_stn(st0_opnd, 0);
    la_calcute_float64(pir1, stn_opnd, st0_opnd, true);
    save_float80_to_stn(stn_opnd, stn);
    la_fpu_pop();

    latxs_ra_free_temp(&stn_opnd);
    latxs_ra_free_temp(&st0_opnd);
}

bool latxs_translate_fadd_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x2) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
            latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);
            int opnd_size = ir1_opnd_size(src);

            gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, false);
            latxs_ra_free_temp(&mem_opnd);
            return true;
        }

        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0(pir1, opnd_num, stn);
        return true;
    } else {
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            int opnd_size = ir1_opnd_size(src);
            if (opnd_size == 32) {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_flds_FT0, HELPER_FLDS_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fadd_ST0_FT0,
                                                 default_helper_cfg, HELPER_FADD_ST0_FT0);

                latxs_ra_free_temp(&temp);
                return true;
            } else {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_fldl_FT0, HELPER_FLDL_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fadd_ST0_FT0,
                                                 default_helper_cfg, HELPER_FADD_ST0_FT0);

                latxs_ra_free_temp(&temp);
                return true;
            }
            lsassert(0);
        } else if (opnd_num == 1) {
			fmov_FT0_STN(ir1_opnd_base_reg_num(src));
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fadd_ST0_FT0,
                                             default_helper_cfg, HELPER_FADD_ST0_FT0);
            return true;
        } else if (opnd_num == 2) {
            latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fadd_STN_ST0,
                                             ir1_opnd_base_reg_num(src),
                                             default_helper_cfg, HELPER_FADD_STN_ST0);
            return true;
        }
    }
    lsassert(0);
    return false;
}

bool latxs_translate_faddp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x1) {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0_pop(pir1, stn);
    } else {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fadd_STN_ST0,
                                         ir1_opnd_base_reg_num(src),
                                         default_helper_cfg, HELPER_FADD_STN_ST0);
        la_fpu_pop();
    }

    return true;
}

bool latxs_translate_fiadd_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x40) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);
        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

        gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, true);
        latxs_ra_free_temp(&mem_opnd);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);

        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
        latxs_tr_gen_call_to_helper((ADDR)helper_fildl_FT0, HELPER_FILDL_FT0);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fadd_ST0_FT0,
                                         default_helper_cfg, HELPER_FADD_ST0_FT0);

        latxs_ra_free_temp(&temp);
    }


    return true;
}

bool latxs_translate_fmul_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x1000) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
            latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);
            int opnd_size = ir1_opnd_size(src);

            gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, false);
            latxs_ra_free_temp(&mem_opnd);
            return true;
        }

        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0(pir1, opnd_num, stn);
        return true;
    } else {
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            int opnd_size = ir1_opnd_size(src);
            if (opnd_size == 32) {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_flds_FT0, HELPER_FLDS_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fmul_ST0_FT0,
                                                 default_helper_cfg, HELPER_FMUL_ST0_FT0);
                latxs_ra_free_temp(&temp);
                return true;
            } else {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_fldl_FT0, HELPER_FLDL_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fmul_ST0_FT0,
                                                 default_helper_cfg, HELPER_FMUL_ST0_FT0);
                latxs_ra_free_temp(&temp);
                return true;
            }
            lsassert(0);
        } else if (opnd_num == 1) {
			fmov_FT0_STN(ir1_opnd_base_reg_num(src));
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fmul_ST0_FT0,
                                             default_helper_cfg, HELPER_FMUL_ST0_FT0);
            return true;
        } else if (opnd_num == 2) {
            latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmul_STN_ST0,
                                             ir1_opnd_base_reg_num(src),
                                             default_helper_cfg, HELPER_FMUL_STN_ST0);
            return true;
        }
    }
    lsassert(0);
    return false;
}

bool latxs_translate_fmulp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x2000) {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0_pop(pir1, stn);
    } else {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fmul_STN_ST0,
                                         ir1_opnd_base_reg_num(src),
                                         default_helper_cfg, HELPER_FMUL_STN_ST0);
        la_fpu_pop();
    }


    return true;
}

bool latxs_translate_fimul_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x200) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);
        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

        gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, true);
        latxs_ra_free_temp(&mem_opnd);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);

        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
        latxs_tr_gen_call_to_helper((ADDR)helper_fildl_FT0, HELPER_FILDL_FT0);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fmul_ST0_FT0,
                                         default_helper_cfg, HELPER_FMUL_ST0_FT0);

        latxs_ra_free_temp(&temp);
    }


    return true;
}

bool latxs_translate_fsub_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x20000) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
            latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);
            int opnd_size = ir1_opnd_size(src);

            gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, false);
            latxs_ra_free_temp(&mem_opnd);
            return true;
        }

        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0(pir1, opnd_num, stn);
        return true;
    } else {
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            int opnd_size = ir1_opnd_size(src);
            if (opnd_size == 32) {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_flds_FT0, HELPER_FLDS_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsub_ST0_FT0,
                                                 default_helper_cfg, HELPER_FSUB_ST0_FT0);

                latxs_ra_free_temp(&temp);
                return true;
            } else {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_fldl_FT0, HELPER_FLDL_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsub_ST0_FT0,
                                                 default_helper_cfg, HELPER_FSUB_ST0_FT0);

                latxs_ra_free_temp(&temp);
                return true;
            }
            lsassert(0);
        } else if (opnd_num == 1) {
			fmov_FT0_STN(ir1_opnd_base_reg_num(src));
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsub_ST0_FT0,
                                             default_helper_cfg, HELPER_FSUB_ST0_FT0);
            return true;
        } else if (opnd_num == 2) {
            latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fsub_STN_ST0,
                                             ir1_opnd_base_reg_num(src),
                                             default_helper_cfg, HELPER_FSUB_STN_ST0);
            return true;
        }
    }
    lsassert(0);
    return false;
}

bool latxs_translate_fsubr_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x80000) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
            latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);
            int opnd_size = ir1_opnd_size(src);

            gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, false);
            latxs_ra_free_temp(&mem_opnd);
            return true;
        }

        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0(pir1, opnd_num, stn);
        return true;

    } else {
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            int opnd_size = ir1_opnd_size(src);
            if (opnd_size == 32) {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_flds_FT0, HELPER_FLDS_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsubr_ST0_FT0,
                                                 default_helper_cfg, HELPER_FSUBR_ST0_FT0);

                latxs_ra_free_temp(&temp);
                return true;
            } else {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_fldl_FT0, HELPER_FLDL_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsubr_ST0_FT0,
                                                 default_helper_cfg, HELPER_FSUBR_ST0_FT0);

                latxs_ra_free_temp(&temp);
                return true;
            }
            lsassert(0);
        } else if (opnd_num == 1) {
			fmov_FT0_STN(ir1_opnd_base_reg_num(src));
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsubr_ST0_FT0,
                                             default_helper_cfg, HELPER_FSUBR_ST0_FT0);
            return true;
        } else if (opnd_num == 2) {
            latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fsubr_STN_ST0,
                                             ir1_opnd_base_reg_num(src),
                                             default_helper_cfg, HELPER_FSUBR_STN_ST0);
            return true;
        }
    }
    lsassert(0);
    return false;
}

bool latxs_translate_fsubp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x40000) {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0_pop(pir1, stn);
    } else {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fsub_STN_ST0,
                                         ir1_opnd_base_reg_num(src),
                                         default_helper_cfg, HELPER_FSUB_STN_ST0);
        la_fpu_pop();
    }

    return true;
}

bool latxs_translate_fsubrp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x100000) {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0_pop(pir1, stn);
    } else {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fsubr_STN_ST0,
                                         ir1_opnd_base_reg_num(src),
                                         default_helper_cfg, HELPER_FSUBR_STN_ST0);
        la_fpu_pop();
    }

    return true;
}

bool latxs_translate_fisub_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x400) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);
        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

        gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, true);
        latxs_ra_free_temp(&mem_opnd);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);

        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
        latxs_tr_gen_call_to_helper((ADDR)helper_fildl_FT0, HELPER_FILDL_FT0);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsub_ST0_FT0,
                                         default_helper_cfg, HELPER_FSUB_ST0_FT0);

        latxs_ra_free_temp(&temp);
    }

    return true;
}

bool latxs_translate_fisubr_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x800) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);
        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

        gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, true);
        latxs_ra_free_temp(&mem_opnd);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);

        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
        latxs_tr_gen_call_to_helper((ADDR)helper_fildl_FT0, HELPER_FILDL_FT0);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsubr_ST0_FT0,
                                         default_helper_cfg, HELPER_FSUBR_ST0_FT0);

        latxs_ra_free_temp(&temp);
    }

    return true;
}

bool latxs_translate_fdiv_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x4) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
            latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);
            int opnd_size = ir1_opnd_size(src);

            gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, false);
            latxs_ra_free_temp(&mem_opnd);
            return true;
        }

        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0(pir1, opnd_num, stn);
        return true;
    } else {
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            int opnd_size = ir1_opnd_size(src);
            if (opnd_size == 32) {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_flds_FT0, HELPER_FLDS_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fdiv_ST0_FT0,
                                                 default_helper_cfg, HELPER_FDIV_ST0_FT0);

                latxs_ra_free_temp(&temp);
                return true;
            } else {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_fldl_FT0, HELPER_FLDL_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fdiv_ST0_FT0,
                                                 default_helper_cfg, HELPER_FDIV_ST0_FT0);

                latxs_ra_free_temp(&temp);
                return true;
            }
            lsassert(0);
        } else if (opnd_num == 1) {
			fmov_FT0_STN(ir1_opnd_base_reg_num(src));
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fdiv_ST0_FT0,
                                             default_helper_cfg, HELPER_FDIV_ST0_FT0);
            return true;
        } else if (opnd_num == 2) {
            latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fdiv_STN_ST0,
                                             ir1_opnd_base_reg_num(src),
                                             default_helper_cfg, HELPER_FDIV_STN_ST0);
            return true;
        }
    }
    lsassert(0);
    return false;
}

bool latxs_translate_fdivr_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x10) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
            latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);
            int opnd_size = ir1_opnd_size(src);

            gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, false);
            latxs_ra_free_temp(&mem_opnd);
            return true;
        }

        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0(pir1, opnd_num, stn);
        return true;

    } else {
        int opnd_num = ir1_opnd_num(pir1);
        lsassert(opnd_num == 1 || opnd_num == 2);

        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        if (ir1_opnd_is_mem(src)) {
            int opnd_size = ir1_opnd_size(src);
            if (opnd_size == 32) {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_flds_FT0, HELPER_FLDS_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fdivr_ST0_FT0,
                                                 default_helper_cfg, HELPER_FDIVR_ST0_FT0);

                latxs_ra_free_temp(&temp);
                return true;
            } else {
                IR2_OPND temp = latxs_ra_alloc_itemp();
                latxs_load_ir1_to_ir2(&temp, src, EXMode_S);
                latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                        &latxs_env_ir2_opnd);
                if (hamt_enabled()) {
                    latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
                }
                latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
                latxs_tr_gen_call_to_helper((ADDR)helper_fldl_FT0, HELPER_FLDL_FT0);
                latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
                latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fdivr_ST0_FT0,
                                                 default_helper_cfg, HELPER_FDIVR_ST0_FT0);

                latxs_ra_free_temp(&temp);
                return true;
            }
            lsassert(0);
        } else if (opnd_num == 1) {
			fmov_FT0_STN(ir1_opnd_base_reg_num(src));
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fdivr_ST0_FT0,
                                             default_helper_cfg, HELPER_FDIVR_ST0_FT0);
            return true;
        } else if (opnd_num == 2) {
            latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fdivr_STN_ST0,
                                             ir1_opnd_base_reg_num(src),
                                             default_helper_cfg, HELPER_FDIVR_STN_ST0);
            return true;
        }
    }
    lsassert(0);
    return false;
}

bool latxs_translate_fdivp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x8) {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0_pop(pir1, stn);
    } else {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fdiv_STN_ST0,
                                         ir1_opnd_base_reg_num(src),
                                         default_helper_cfg, HELPER_FDIV_STN_ST0);
        la_fpu_pop();
    }

    return true;
}

bool latxs_translate_fdivrp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x20) {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        int stn = ir1_opnd_base_reg_num(src);
        gen_float64_STN_ST0_pop(pir1, stn);
    } else {
        IR1_OPND *src = ir1_get_opnd(pir1, 0);
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_fdivr_STN_ST0,
                                         ir1_opnd_base_reg_num(src),
                                         default_helper_cfg, HELPER_FDIVR_STN_ST0);
        la_fpu_pop();
    }

    return true;
}

bool latxs_translate_fidiv_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x80) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);
        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

        gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, true);
        latxs_ra_free_temp(&mem_opnd);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);

        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
        latxs_tr_gen_call_to_helper((ADDR)helper_fildl_FT0, HELPER_FILDL_FT0);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fdiv_ST0_FT0,
                default_helper_cfg, HELPER_FDIV_ST0_FT0);

        latxs_ra_free_temp(&temp);
    }

    return true;
}

bool latxs_translate_fidivr_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x100) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);
        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

        gen_float64_ST0_MEM(pir1, mem_opnd, opnd_size, true);
        latxs_ra_free_temp(&mem_opnd);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);

        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
        latxs_tr_gen_call_to_helper((ADDR)helper_fildl_FT0, HELPER_FILDL_FT0);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fdivr_ST0_FT0,
                                         default_helper_cfg, HELPER_FDIVR_ST0_FT0);

        latxs_ra_free_temp(&temp);
    }

    return true;
}

bool latxs_translate_fabs_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        IR2_OPND high_opnd = latxs_ra_alloc_itemp();

        IR2_OPND stn_opnd_addr = get_stn_opnd_addr(0);
        latxs_append_ir2_opnd2i(LISA_LD_HU, &high_opnd, &stn_opnd_addr, 8);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &high_opnd, &high_opnd, 14, 0);
        latxs_append_ir2_opnd2i(LISA_ST_H, &high_opnd, &stn_opnd_addr, 8);

        latxs_ra_free_temp(&stn_opnd_addr);
        latxs_ra_free_temp(&high_opnd);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fabs_ST0, default_helper_cfg,
                HELPER_FABS_ST0);
    }
    return true;
}

bool latxs_translate_fchs_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        IR2_OPND high_opnd = latxs_ra_alloc_itemp();
        IR2_OPND itemp = latxs_ra_alloc_itemp();

        IR2_OPND stn_opnd_addr = get_stn_opnd_addr(0);

        latxs_append_ir2_opnd2i(LISA_LD_HU, &high_opnd, &stn_opnd_addr, 8);
        li_wu(itemp, 0x8000);
        latxs_append_ir2_opnd3(LISA_XOR, &high_opnd, &high_opnd, &itemp);
        latxs_append_ir2_opnd2i(LISA_ST_H, &high_opnd, &stn_opnd_addr, 8);

        latxs_ra_free_temp(&stn_opnd_addr);
        latxs_ra_free_temp(&itemp);
        latxs_ra_free_temp(&high_opnd);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fchs_ST0, default_helper_cfg,
                HELPER_FCHS_ST0);
    }
    return true;
}

bool latxs_translate_fsqrt_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x10000) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);
        la_calcute_float64(pir1, st0_opnd, st0_opnd, false);
        save_float80_to_stn(st0_opnd, 0);

        latxs_ra_free_temp(&st0_opnd);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsqrt, default_helper_cfg,
                HELPER_FSQRT);
    }

    return true;
}

bool latxs_translate_fprem_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fprem, nofp_helper_cfg,
                HELPER_FPREM);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fprem, default_helper_cfg,
                HELPER_FPREM);
    }

    return true;
}

bool latxs_translate_fprem1_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fprem1, nofp_helper_cfg,
                HELPER_FPREM1);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fprem1, default_helper_cfg,
                HELPER_FPREM1);
    }
    return true;
}

bool latxs_translate_frndint_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x4000) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);
        la_calcute_float64(pir1, st0_opnd, st0_opnd, false);
        save_float80_to_stn(st0_opnd, 0);

        latxs_ra_free_temp(&st0_opnd);

    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_frndint, default_helper_cfg,
                HELPER_FRNDINT);
    }

    return true;
}

bool latxs_translate_fxtract_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fxtract, nofp_helper_cfg,
                HELPER_FXTRACT);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fxtract, default_helper_cfg,
                HELPER_FXTRACT);
    }

    return true;
}

static void la_convert_float64_float80(IR2_OPND st)
{
    IR2_OPND ftemp = latxs_ra_alloc_ftemp();

    latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &st, 0x4);
    latxs_append_ir2_opnd3(LISA_FCVT_D_LD, &st, &st, &ftemp);

    latxs_ra_free_temp(&ftemp);

}

static void la_st0_ft0_is_snan(IR2_OPND st0, IR2_OPND ft0, IR2_OPND cond)
{
    IR2_OPND itemp = latxs_ra_alloc_itemp();
    IR2_OPND imm = latxs_ra_alloc_itemp();
    IR2_OPND ftemp = latxs_ra_alloc_ftemp();
    IR2_OPND label_next = latxs_ir2_opnd_new_label();
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();

    li_d(cond, 0);
    latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &st0, 0x4);
    latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &ftemp);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 14, 0);
    li_d(imm, 0x7fff);
    latxs_append_ir2_opnd3(LISA_BNE, &itemp, &imm, &label_next);
    latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &st0);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &imm, &itemp, 62, 62);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &itemp, &latxs_zero_ir2_opnd, 63, 62);
    latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_next);
    latxs_append_ir2_opnd3(LISA_BNE, &imm, &latxs_zero_ir2_opnd, &label_next);
    li_d(cond, 1);
    latxs_append_ir2_opnd1(LISA_B, &label_exit);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_next);
    latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &ft0, 0x4);
    latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &ftemp);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 14, 0);
    li_d(imm, 0x7fff);
    latxs_append_ir2_opnd3(LISA_BNE, &itemp, &imm, &label_exit);
    latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &ft0);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &imm, &itemp, 62, 62);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &itemp, &latxs_zero_ir2_opnd, 63, 62);
    latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_exit);
    latxs_append_ir2_opnd3(LISA_BNE, &imm, &latxs_zero_ir2_opnd, &label_exit);
    li_d(cond, 1);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    latxs_ra_free_temp(&itemp);
    latxs_ra_free_temp(&imm);
    latxs_ra_free_temp(&ftemp);
}

static void la_float80_invalid_encoding(IR2_OPND stn, IR2_OPND cond, IR2_OPND label_invalid)
{
    IR2_OPND itemp = latxs_ra_alloc_itemp();
    IR2_OPND imm = latxs_ra_alloc_itemp();
    IR2_OPND ftemp = latxs_ra_alloc_ftemp();

    IR2_OPND label_no_invalid1 = latxs_ir2_opnd_new_label();
    IR2_OPND label_no_invalid2 = latxs_ir2_opnd_new_label();

    li_d(cond, 0);
    latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &stn);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 63, 63);
    latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_no_invalid1);

    latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &stn, 0x4);
    latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &ftemp);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 14, 0);
    latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_no_invalid1);
    li_d(cond, 1);
    latxs_append_ir2_opnd1(LISA_B, &label_invalid);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_no_invalid1);
    li_d(imm, 0x7fff);
    latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &stn, 0x4);
    latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &ftemp);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 14, 0);
    latxs_append_ir2_opnd3(LISA_BNE, &itemp, &imm, &label_no_invalid2);
    latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &stn);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &itemp, &latxs_zero_ir2_opnd, 63, 63);
    latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_invalid);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_no_invalid2);

    latxs_ra_free_temp(&itemp);
    latxs_ra_free_temp(&imm);
    latxs_ra_free_temp(&ftemp);
}

#define C0_BIT (8)
#define C2_BIT (10)
#define C3_BIT (14)

static void la_fcom_ST0_FT0(IR2_OPND st0, IR2_OPND ft0, bool is_eflags, bool is_quiet, bool is_float64)
{
    int C0_CF, C2_PF, C3_ZF;
    if (is_eflags) {
        C0_CF = 0;
        C2_PF = 2;
        C3_ZF = 6;
    } else {
        C0_CF = C0_BIT;
        C2_PF = C2_BIT;
        C3_ZF = C3_BIT;
    }

    IR2_OPND fpus_eflags = latxs_ra_alloc_itemp();
    IR2_OPND itemp = latxs_ra_alloc_itemp();
    IR2_OPND label_invalid = latxs_ir2_opnd_new_label();
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();

    if (is_eflags) {
        latxs_append_ir2_opnd1i(LISA_X86MFFLAG, &fpus_eflags, 0x3f);
    } else {
        latxs_append_ir2_opnd2i(LISA_LD_H, &fpus_eflags, &latxs_env_ir2_opnd,
                                lsenv_offset_of_status_word(lsenv));
    }

    la_float80_invalid_encoding(st0, itemp, label_invalid);
    la_float80_invalid_encoding(ft0, itemp, label_invalid);

    if (is_float64) {
        /* use float64 */
        la_convert_float64_float80(st0);
        la_convert_float64_float80(ft0);
        IR2_OPND label_for_ft0_cle_st0 = latxs_ir2_opnd_new_label();
        IR2_OPND label_for_ft0_ceq_st0 = latxs_ir2_opnd_new_label();
        IR2_OPND number_1 = latxs_ra_alloc_itemp();

        li_d(number_1, 0x1);

        latxs_append_ir2_opnd3i(LISA_FCMP_COND_D, &latxs_fcc0_ir2_opnd,
                                &ft0, &st0, FCMP_COND_CLE);
        latxs_append_ir2_opnd2(LISA_BCNEZ, &latxs_fcc0_ir2_opnd, &label_for_ft0_cle_st0);

        /* else if (st0 < ft0), set CF and exit*/
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &number_1, C0_CF, C0_CF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C2_PF, C2_PF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C3_ZF, C3_ZF);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        /* LABEL: label_for_src_cle_st0 if(st0 == src), */
        /* set fcc0 then jmp to label label_for_src_ceq_st0*/
        latxs_append_ir2_opnd1(LISA_LABEL, &label_for_ft0_cle_st0);
        latxs_append_ir2_opnd3i(LISA_FCMP_COND_D, &latxs_fcc0_ir2_opnd,
                                &ft0, &st0, FCMP_COND_CEQ);
        latxs_append_ir2_opnd2(LISA_BCNEZ, &latxs_fcc0_ir2_opnd, &label_for_ft0_ceq_st0);

        /* else if (st0 > sti), ZF, PF and CF are all clear, */
        /* just exit directly */
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C0_CF, C0_CF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C2_PF, C2_PF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C3_ZF, C3_ZF);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        /* LABEL: label_for_src_ceq_st0, set ZF if (st0 == src) and exit*/
        latxs_append_ir2_opnd1(LISA_LABEL, &label_for_ft0_ceq_st0);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C0_CF, C0_CF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C2_PF, C2_PF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &number_1, C3_ZF, C3_ZF);

        latxs_ra_free_temp(&number_1);
    } else {
        /* use asm  <==> don't use float64 */
        IR2_OPND itemp2 = latxs_ra_alloc_itemp();
        IR2_OPND number_1 = latxs_ra_alloc_itemp();
        IR2_OPND st0_high = latxs_ra_alloc_itemp();
        IR2_OPND ft0_high = latxs_ra_alloc_itemp();
        IR2_OPND ftemp = latxs_ra_alloc_ftemp();
        IR2_OPND ftemp2 = latxs_ra_alloc_ftemp();

        IR2_OPND label_same_sign = latxs_ir2_opnd_new_label();
        IR2_OPND label_st0_normal = latxs_ir2_opnd_new_label();
        IR2_OPND label_ft0_normal = latxs_ir2_opnd_new_label();
        IR2_OPND label_ft0_lt_st0 = latxs_ir2_opnd_new_label();
        IR2_OPND label_st0_lt_ft0 = latxs_ir2_opnd_new_label();
        IR2_OPND label_st0_ne_ft0 = latxs_ir2_opnd_new_label();
        IR2_OPND label_lt128_true = latxs_ir2_opnd_new_label();
        IR2_OPND label_lt128_false = latxs_ir2_opnd_new_label();
        IR2_OPND label_is_equal = latxs_ir2_opnd_new_label();

        li_d(number_1, 0x1);

        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &st0, 0x4);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &ftemp);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &ft0, 0x4);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp2, &ftemp);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 15, 15);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp2, &itemp2, 15, 15);
        latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &itemp2, &label_same_sign);

        /* aSign != bSign */
        latxs_append_ir2_opnd3(LISA_VOR_V, &ftemp, &st0, &ft0);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &ftemp);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp2, &ftemp, 0x4);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp2, &ftemp2);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp2, &itemp2, 14, 0);
        latxs_append_ir2_opnd3(LISA_OR, &itemp, &itemp, &itemp2);
        latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_is_equal);

        /* get aSign */
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &st0, 0x4);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &ftemp);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 15, 15);
        latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_ft0_lt_st0);
        latxs_append_ir2_opnd1(LISA_B, &label_st0_lt_ft0);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_same_sign);


        /* st0 ==> Normalize pseudo-denormals before comparison  */
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &st0, 0x4);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &st0_high, &ftemp);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &st0_high, &st0_high, 15, 0);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &st0_high, 14, 0);
        latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_st0_normal);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &st0);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 63, 63);
        latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_st0_normal);
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &st0_high, &st0_high, 1);
        latxs_append_ir2_opnd1(LISA_LABEL, &label_st0_normal);

        /* ft0 ==> Normalize pseudo-denormals before comparison  */
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &ft0, 0x4);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &ft0_high, &ftemp);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &ft0_high, &ft0_high, 15, 0);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &ft0_high, 14, 0);
        latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_ft0_normal);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &ft0);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 63, 63);
        latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_ft0_normal);
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &ft0_high, &ft0_high, 1);
        latxs_append_ir2_opnd1(LISA_LABEL, &label_ft0_normal);

        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &st0);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp2, &ft0);
        latxs_append_ir2_opnd3(LISA_BNE, &itemp, &itemp2, &label_st0_ne_ft0);
        latxs_append_ir2_opnd3(LISA_BEQ, &st0_high, &ft0_high, &label_is_equal);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_st0_ne_ft0);
        latxs_append_ir2_opnd3(LISA_BLTU, &st0_high, &ft0_high, &label_lt128_true);
        latxs_append_ir2_opnd3(LISA_BNE, &st0_high, &ft0_high, &label_lt128_false);
        latxs_append_ir2_opnd3(LISA_BLTU, &itemp, &itemp2, &label_lt128_true);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_lt128_false);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp2, &st0_high, 15, 15);
        latxs_append_ir2_opnd2i(LISA_XORI, &itemp2, &itemp2, 0x1);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &st0_high, &itemp2, 15, 15);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_lt128_true);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &st0_high, 15, 15);
        latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_st0_lt_ft0);

        /* st0 > ft0 */
        latxs_append_ir2_opnd1(LISA_LABEL, &label_ft0_lt_st0);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C0_CF, C0_CF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C2_PF, C2_PF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C3_ZF, C3_ZF);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        /* st0 < ft0 */
        latxs_append_ir2_opnd1(LISA_LABEL, &label_st0_lt_ft0);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &number_1, C0_CF, C0_CF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C2_PF, C2_PF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C3_ZF, C3_ZF);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        /* st0 == ft0 */
        latxs_append_ir2_opnd1(LISA_LABEL, &label_is_equal);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C0_CF, C0_CF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &latxs_zero_ir2_opnd, C2_PF, C2_PF);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &number_1, C3_ZF, C3_ZF);

        latxs_ra_free_temp(&itemp2);
        latxs_ra_free_temp(&number_1);
        latxs_ra_free_temp(&st0_high);
        latxs_ra_free_temp(&ft0_high);
        latxs_ra_free_temp(&ftemp);
        latxs_ra_free_temp(&ftemp2);
    }

    latxs_append_ir2_opnd1(LISA_B, &label_exit);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_invalid);
    IR2_OPND number_1 = latxs_ra_alloc_itemp();

    li_d(number_1, 0x1);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &number_1, C0_CF, C0_CF);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &number_1, C2_PF, C2_PF);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &number_1, C3_ZF, C3_ZF);
    if (is_quiet) {
        IR2_OPND label_set_invalid = latxs_ir2_opnd_new_label();

        latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_set_invalid);
        la_st0_ft0_is_snan(st0, ft0, itemp);
        latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_exit);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_set_invalid);
    }

    int fp_status_offset = lsenv_offset_of_fp_status(lsenv);
    int exception_flags_offset = fp_status_offset +
        offsetof(float_status, float_exception_flags);

    latxs_append_ir2_opnd2i(LISA_LD_H, &itemp, &latxs_env_ir2_opnd, exception_flags_offset);

    /* status->float_exception_flags |= float_flag_invalid */
    latxs_append_ir2_opnd2i(LISA_ORI, &itemp, &itemp, 0x1);
    latxs_append_ir2_opnd2i(LISA_ST_H, &itemp, &latxs_env_ir2_opnd, exception_flags_offset);

    IR2_OPND fpus;
    if (is_eflags) {
        fpus = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_LD_H, &fpus, &latxs_env_ir2_opnd, lsenv_offset_of_status_word(lsenv));
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus, &number_1, X87_SR_OFF_IE, X87_SR_OFF_IE);
    } else {
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &fpus_eflags, &number_1, X87_SR_OFF_IE, X87_SR_OFF_IE);
    }

    latxs_append_ir2_opnd2i(LISA_LD_H, &itemp, &latxs_env_ir2_opnd, lsenv_offset_of_control_word(lsenv));
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, X87_CR_OFF_IM, X87_CR_OFF_IM);
    latxs_append_ir2_opnd2i(LISA_XORI, &itemp, &itemp, 0x1);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &itemp, &itemp, 8, 8);
    latxs_append_ir2_opnd2i(LISA_SLLI_D, &itemp, &itemp, 7);

    if (is_eflags) {
        latxs_append_ir2_opnd3(LISA_OR, &fpus, &fpus, &itemp);
        latxs_append_ir2_opnd2i(LISA_ST_H, &fpus, &latxs_env_ir2_opnd, lsenv_offset_of_status_word(lsenv));
        latxs_ra_free_temp(&fpus);
    } else {
        latxs_append_ir2_opnd3(LISA_OR, &fpus_eflags, &fpus_eflags, &itemp);
    }

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    if (is_eflags) {
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &fpus_eflags, 0x3f);
    } else {
        latxs_append_ir2_opnd2i(LISA_ST_H, &fpus_eflags, &latxs_env_ir2_opnd,
                                lsenv_offset_of_status_word(lsenv));
    }

    latxs_ra_free_temp(&itemp);
    latxs_ra_free_temp(&fpus_eflags);
    latxs_ra_free_temp(&number_1);

}

bool latxs_translate_fcom_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ft0_opnd = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);
        if (ir1_opnd_is_fpr(opnd)) {
            get_float80_from_stn(ft0_opnd, ir1_opnd_base_reg_num(opnd));
        } else {
            IR2_OPND src_opnd = latxs_ra_alloc_ftemp();
            IR2_OPND ftemp = latxs_ra_alloc_ftemp();

            latxs_load_freg_from_ir1_2(&src_opnd, opnd, true);
            latxs_append_ir2_opnd2(LISA_FCVT_UD_D, &ftemp, &src_opnd);
            latxs_append_ir2_opnd2(LISA_FCVT_LD_D, &ft0_opnd, &src_opnd);
            latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ft0_opnd, &ftemp, 0x40);

            latxs_ra_free_temp(&src_opnd);
            latxs_ra_free_temp(&ftemp);
        }

        if (option_soft_fpu_fast & 0x1000000) {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, false, true);
        } else {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, false, false);
        }
        return true;

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd);
        if (ir1_opnd_is_fpr(opnd)) {
			fmov_FT0_STN(ir1_opnd_base_reg_num(opnd));

            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcom_ST0_FT0,
                    default_helper_cfg, HELPER_FCOM_ST0_FT0);
            return true;
        } else if (opnd_size == 32) {
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
            latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                    &latxs_env_ir2_opnd);
            if (hamt_enabled()) {
                latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
            }
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
            latxs_tr_gen_call_to_helper((ADDR)helper_flds_FT0, HELPER_FLDS_FT0);
            latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcom_ST0_FT0,
                    default_helper_cfg, HELPER_FCOM_ST0_FT0);
            latxs_ra_free_temp(&temp);
            return true;
        } else if (opnd_size == 64) {
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
            latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                    &latxs_env_ir2_opnd);
            if (hamt_enabled()) {
                latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
            }
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
            latxs_tr_gen_call_to_helper((ADDR)helper_fldl_FT0, HELPER_FLDL_FT0);
            latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
            latxs_ra_free_temp(&temp);
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcom_ST0_FT0,
                    default_helper_cfg, HELPER_FCOM_ST0_FT0);
            return true;
        }
    }
    lsassert(0);
    return false;
}

bool latxs_translate_fcomp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_translate_fcom_softfpu(pir1);
        la_fpu_pop();
        return true;

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd);
        if (ir1_opnd_is_fpr(opnd)) {
			fmov_FT0_STN(ir1_opnd_base_reg_num(opnd));

            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcom_ST0_FT0,
                    default_helper_cfg, HELPER_FCOM_ST0_FT0);
            la_fpu_pop();
            return true;
        } else if (opnd_size == 32) {
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
            latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                    &latxs_env_ir2_opnd);
            if (hamt_enabled()) {
                latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
            }
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
            latxs_tr_gen_call_to_helper((ADDR)helper_flds_FT0, HELPER_FLDS_FT0);
            latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcom_ST0_FT0,
                    default_helper_cfg, HELPER_FCOM_ST0_FT0);
            la_fpu_pop();

            latxs_ra_free_temp(&temp);
            return true;
        } else if (opnd_size == 64) {
            IR2_OPND temp = latxs_ra_alloc_itemp();
            latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
            latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                    &latxs_env_ir2_opnd);
            if (hamt_enabled()) {
                latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
            }
            latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
            latxs_tr_gen_call_to_helper((ADDR)helper_fldl_FT0, HELPER_FLDL_FT0);
            latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
            latxs_ra_free_temp(&temp);
            latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcom_ST0_FT0,
                    default_helper_cfg, HELPER_FCOM_ST0_FT0);
            la_fpu_pop();
            return true;
        }
    }
    lsassert(0);
    return false;
}
bool latxs_translate_fcompp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    int reg_num = 1;

    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ft0_opnd = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);
        get_float80_from_stn(ft0_opnd, reg_num);

        if (option_soft_fpu_fast & 0x1000000) {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, false, true);
        } else {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, false, false);
        }
        la_fpu_pop();
        la_fpu_pop();
    } else {
		fmov_FT0_STN(reg_num);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcom_ST0_FT0,
                default_helper_cfg, HELPER_FCOM_ST0_FT0);
        la_fpu_pop();
        la_fpu_pop();
    }

    return true;
}

bool latxs_translate_fucom_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    int reg_num = ir1_opnd_base_reg_num(opnd0);

    if (option_soft_fpu_fast) {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);

        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ft0_opnd = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);
        get_float80_from_stn(ft0_opnd, ir1_opnd_base_reg_num(opnd));

        if (option_soft_fpu_fast & 0x2000000) {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, true, true);
        } else {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, true, false);
        }
        return true;
    } else {
		fmov_FT0_STN(reg_num);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fucom_ST0_FT0,
                default_helper_cfg, HELPER_FUCOM_ST0_FT0);
    }

    return true;
}

bool latxs_translate_fucomp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    int reg_num = ir1_opnd_base_reg_num(opnd0);
    
    if (option_soft_fpu_fast) {
        latxs_translate_fucom_softfpu(pir1);
        la_fpu_pop();
    } else {
		fmov_FT0_STN(reg_num);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fucom_ST0_FT0,
                default_helper_cfg, HELPER_FUCOM_ST0_FT0);
        la_fpu_pop();
    }

    return true;
}
bool latxs_translate_fucompp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ft0_opnd = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);
        get_float80_from_stn(ft0_opnd, 1);

        if (option_soft_fpu_fast & 0x2000000) {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, true, true);
        } else {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, true, false);
        }
        la_fpu_pop();
        la_fpu_pop();
    } else {
		fmov_FT0_STN(1);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fucom_ST0_FT0,
                default_helper_cfg, HELPER_FUCOM_ST0_FT0);
        la_fpu_pop();
        la_fpu_pop();
    }

    return true;
}

bool latxs_translate_fcomi_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_zero_ir2_opnd, 0x34);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    int reg_num = ir1_opnd_base_reg_num(opnd0);

    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ft0_opnd = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);
        get_float80_from_stn(ft0_opnd, reg_num);

        if (option_soft_fpu_fast & 0x4000000) {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, true, false, true);
        } else {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, true, false, false);
        }

    } else {
		fmov_FT0_STN(reg_num);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcomi_ST0_FT0,
                default_helper_cfg, HELPER_FCOMI_ST0_FT0);
    }

    return true;
}
bool latxs_translate_fcomip_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_zero_ir2_opnd, 0x34);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    int reg_num = ir1_opnd_base_reg_num(opnd0);
    if (option_soft_fpu_fast) {
        latxs_translate_fcomi_softfpu(pir1);
        la_fpu_pop();

    } else {
		fmov_FT0_STN(reg_num);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcomi_ST0_FT0,
                default_helper_cfg, HELPER_FCOMI_ST0_FT0);
        la_fpu_pop();
    }

    return true;
}
bool latxs_translate_fucomi_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_zero_ir2_opnd, 0x34);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    int reg_num = ir1_opnd_base_reg_num(opnd0);
    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ft0_opnd = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);
        get_float80_from_stn(ft0_opnd, reg_num);

        if (option_soft_fpu_fast & 0x8000000) {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, true, true, true);
        } else {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, true, true, false);
        }

    } else {
		fmov_FT0_STN(reg_num);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fucomi_ST0_FT0,
                default_helper_cfg, HELPER_FUCOMI_ST0_FT0);
    }

    return true;
}
bool latxs_translate_fucomip_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_zero_ir2_opnd, 0x34);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    int reg_num = ir1_opnd_base_reg_num(opnd0);
    if (option_soft_fpu_fast) {
        latxs_translate_fucomi_softfpu(pir1);
        la_fpu_pop();

    } else {
		fmov_FT0_STN(reg_num);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fucomi_ST0_FT0,
                default_helper_cfg, HELPER_FUCOMI_ST0_FT0);
        la_fpu_pop();
    }

    return true;
}

bool latxs_translate_ficom_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
    IR2_OPND temp = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ft0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND src_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ftemp = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);

        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &src_opnd, &temp);
        latxs_append_ir2_opnd2(LISA_FFINT_D_W, &src_opnd, &src_opnd);

        latxs_append_ir2_opnd2(LISA_FCVT_UD_D, &ftemp, &src_opnd);
        latxs_append_ir2_opnd2(LISA_FCVT_LD_D, &ft0_opnd, &src_opnd);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ft0_opnd, &ftemp, 0x40);

        latxs_ra_free_temp(&temp);
        if (option_soft_fpu_fast & 0x1000000) {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, false, true);
        } else {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, false, false);
        }

        latxs_ra_free_temp(&src_opnd);
        latxs_ra_free_temp(&ftemp);

    } else {
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
        latxs_tr_gen_call_to_helper((ADDR)helper_fildl_FT0, HELPER_FILDL_FT0);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcom_ST0_FT0,
                                         default_helper_cfg, HELPER_FCOM_ST0_FT0);
        latxs_ra_free_temp(&temp);
    }

    return true;
}

bool latxs_translate_ficomp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
    IR2_OPND temp = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&temp, opnd, EXMode_S);
    if (option_soft_fpu_fast) {
        latxs_translate_ficom_softfpu(pir1);
        la_fpu_pop();

    } else {
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
        latxs_tr_gen_call_to_helper((ADDR)helper_fildl_FT0, HELPER_FILDL_FT0);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcom_ST0_FT0,
                                         default_helper_cfg, HELPER_FCOM_ST0_FT0);
        la_fpu_pop();
    }
    latxs_ra_free_temp(&temp);

    return true;
}

bool latxs_translate_ftst_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND ft0_opnd = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);
        latxs_append_ir2_opnd2i(LISA_VANDI_B, &ft0_opnd, &ft0_opnd, 0);

        if (option_soft_fpu_fast & 0x1000000) {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, false, true);
        } else {
            la_fcom_ST0_FT0(st0_opnd, ft0_opnd, false, false, false);
        }
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fldz_FT0, default_helper_cfg,
                HELPER_FLDZ_FT0);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcom_ST0_FT0,
                default_helper_cfg, HELPER_FCOM_ST0_FT0);
    }
    return true;
}

bool latxs_translate_fxam_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND fpr_st0_opnd = latxs_ra_alloc_ftemp();
        get_float80_from_stn(fpr_st0_opnd, 0);

        IR2_OPND ftemp = latxs_ra_alloc_ftemp();
        IR2_OPND gpr_st0_high_opnd = latxs_ra_alloc_itemp();

        IR2_OPND fpu_tag = latxs_ra_alloc_itemp();
        IR2_OPND top_opnd = latxs_ra_alloc_itemp();
        IR2_OPND fpus = latxs_ra_alloc_itemp();
        IR2_OPND temp1 = latxs_ra_alloc_itemp();
        IR2_OPND temp2 = latxs_ra_alloc_itemp();
        int status_offset = lsenv_offset_of_status_word(lsenv);
        IR2_OPND not_set_c1 = latxs_ir2_opnd_new_label();
        IR2_OPND label_next = latxs_ir2_opnd_new_label();
        IR2_OPND label_exit = latxs_ir2_opnd_new_label();
        IR2_OPND label_infinity_nan = latxs_ir2_opnd_new_label();
        IR2_OPND label_zero_denormal = latxs_ir2_opnd_new_label();
        IR2_OPND label_infinity = latxs_ir2_opnd_new_label();
        IR2_OPND label_zero = latxs_ir2_opnd_new_label();


        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &ftemp, &latxs_zero_ir2_opnd);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &ftemp, &fpr_st0_opnd, 0x4);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &gpr_st0_high_opnd, &ftemp);
        /*
         * C3, C2, C1, C0 ← 0000
         */
        li_wu(temp1, 0xb8ff);
        latxs_append_ir2_opnd2i(LISA_LD_H, &fpus, &latxs_env_ir2_opnd, status_offset);
        latxs_append_ir2_opnd3(LISA_AND, &fpus, &fpus, &temp1);

        /*
         * C1 ← sign bit of ST0
         */
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &temp2, &gpr_st0_high_opnd, 15, 15);
        latxs_append_ir2_opnd3(LISA_BEQ, &temp2, &latxs_zero_ir2_opnd, &not_set_c1);
        latxs_append_ir2_opnd2i(LISA_ORI, &fpus, &fpus, 0x200);
        latxs_append_ir2_opnd1(LISA_LABEL, &not_set_c1);

        /* empty is judged by FPU tag word */
        int tag_offset = lsenv_offset_of_tag_word(lsenv);
        lsassert(tag_offset <= 0x7ff);
        latxs_append_ir2_opnd2i(LISA_LD_D, &fpu_tag, &latxs_env_ir2_opnd, tag_offset);

        latxs_append_ir2_opnd2i(LISA_LD_WU, &top_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));
        li_d(temp1, (uint64)(0xffULL));
        latxs_append_ir2_opnd2i(LISA_SLLI_D, &top_opnd, &top_opnd, 3);
        latxs_append_ir2_opnd3(LISA_SRL_D, &fpu_tag, &fpu_tag, &top_opnd);
        latxs_append_ir2_opnd3(LISA_AND, &fpu_tag, &fpu_tag, &temp1);
        latxs_append_ir2_opnd3(LISA_BEQ, &fpu_tag, &latxs_zero_ir2_opnd, &label_next);

        li_wu(temp1, 0x4100UL);
        latxs_append_ir2_opnd3(LISA_OR, &fpus, &fpus, &temp1);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        latxs_ra_free_temp(&fpu_tag);
        /*
         * slow path
         */
        IR2_OPND gpr_st0_low_opnd = latxs_ra_alloc_itemp();

        latxs_append_ir2_opnd1(LISA_LABEL, &label_next);
        li_wu(temp1, 0x7fffULL);
        latxs_append_ir2_opnd3(LISA_AND, &temp2, &gpr_st0_high_opnd, &temp1);
        latxs_append_ir2_opnd3(LISA_BEQ, &temp2, &temp1, &label_infinity_nan);
        latxs_append_ir2_opnd3(LISA_BEQ, &temp2, &latxs_zero_ir2_opnd, &label_zero_denormal);
        li_d(temp1, 0x8000000000000000ULL);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &gpr_st0_low_opnd, &fpr_st0_opnd);
        latxs_append_ir2_opnd3(LISA_AND, &temp2, &gpr_st0_low_opnd, &temp1);
        /* Unsupported number */
        latxs_append_ir2_opnd3(LISA_BEQ, &temp2, &latxs_zero_ir2_opnd, &label_exit);
        /* Normal finite number */
        latxs_append_ir2_opnd2i(LISA_ORI, &fpus, &fpus, 0x400);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_infinity_nan);
        li_d(temp1, 0x8000000000000000ULL);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &gpr_st0_low_opnd, &fpr_st0_opnd);
        latxs_append_ir2_opnd3(LISA_BEQ, &gpr_st0_low_opnd, &temp1, &label_infinity);
        latxs_append_ir2_opnd3(LISA_AND, &temp2, &gpr_st0_low_opnd, &temp1);
        /* Unsupported number */
        latxs_append_ir2_opnd3(LISA_BEQ, &temp2, &latxs_zero_ir2_opnd, &label_exit);
        /* NaN */
        latxs_append_ir2_opnd2i(LISA_ORI, &fpus, &fpus, 0x100);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        /* Infinity */
        latxs_append_ir2_opnd1(LISA_LABEL, &label_infinity);
        latxs_append_ir2_opnd2i(LISA_ORI, &fpus, &fpus, 0x500);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_zero_denormal);
        li_d(temp1, 0x8000000000000000ULL);
        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &gpr_st0_low_opnd, &fpr_st0_opnd);
        latxs_append_ir2_opnd3(LISA_BEQ, &gpr_st0_low_opnd, &latxs_zero_ir2_opnd, &label_zero);
        /* Denormal */
        li_wu(temp1, 0x4400UL);
        latxs_append_ir2_opnd3(LISA_OR, &fpus, &fpus, &temp1);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);
        /* Zero */
        latxs_append_ir2_opnd1(LISA_LABEL, &label_zero);
        li_w(temp1, 0x4000UL);
        latxs_append_ir2_opnd3(LISA_OR, &fpus, &fpus, &temp1);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);
        /*
         * exit
         */
        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
        latxs_append_ir2_opnd2i(LISA_ST_H, &fpus, &latxs_env_ir2_opnd, status_offset);

        latxs_ra_free_temp(&temp1);
        latxs_ra_free_temp(&temp2);
        latxs_ra_free_temp(&gpr_st0_high_opnd);
        latxs_ra_free_temp(&gpr_st0_low_opnd);
        latxs_ra_free_temp(&top_opnd);
        latxs_ra_free_temp(&fpus);

    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fxam_ST0, default_helper_cfg,
                HELPER_FXAM_ST0);
    }
    return true;
}

bool latxs_translate_fsin_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsin, nofp_helper_cfg, HELPER_FSIN);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsin, default_helper_cfg, HELPER_FSIN);
    }

    return true;
}

bool latxs_translate_fcos_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcos, nofp_helper_cfg, HELPER_FCOS);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fcos, default_helper_cfg, HELPER_FCOS);
    }

    return true;
}

bool latxs_translate_fpatan_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fpatan, nofp_helper_cfg, HELPER_FPATAN);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fpatan, default_helper_cfg, HELPER_FPATAN);
    }

    return true;
}

bool latxs_translate_fsincos_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsincos, nofp_helper_cfg, HELPER_FSINCOS);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fsincos, default_helper_cfg, HELPER_FSINCOS);
    }

    return true;
}

bool latxs_translate_fptan_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fptan, nofp_helper_cfg, HELPER_FPTAN);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fptan, default_helper_cfg, HELPER_FPTAN);
    }

    return true;
}

bool latxs_translate_fyl2x_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fyl2x, nofp_helper_cfg, HELPER_FYL2X);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fyl2x, default_helper_cfg, HELPER_FYL2X);

    }

    return true;
}

bool latxs_translate_fyl2xp1_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fyl2xp1, nofp_helper_cfg, HELPER_FYL2XP1);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fyl2xp1, default_helper_cfg, HELPER_FYL2XP1);
    }

    return true;
}

bool latxs_translate_fscale_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast & 0x8000) {
        IR2_OPND st0_opnd = latxs_ra_alloc_ftemp();
        IR2_OPND st1_opnd = latxs_ra_alloc_ftemp();

        get_float80_from_stn(st0_opnd, 0);
        get_float80_from_stn(st1_opnd, 1);
        la_calcute_float64(pir1, st0_opnd, st1_opnd, true);
        save_float80_to_stn(st0_opnd, 0);

        latxs_ra_free_temp(&st0_opnd);
        latxs_ra_free_temp(&st1_opnd);

    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fscale, default_helper_cfg, HELPER_FSCALE);
    }

    return true;
}

bool latxs_translate_f2xm1_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_f2xm1, nofp_helper_cfg, HELPER_F2XM1);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_f2xm1, default_helper_cfg, HELPER_F2XM1);
    }

    return true;
}

bool latxs_translate_fninit_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND temp = latxs_ra_alloc_itemp();

        /* clear status and set control*/
        int offset = lsenv_offset_of_status_word(lsenv);
        lsassert(offset <= 0x7ff);

        li_wu(temp, 0x37f0000ULL);
        latxs_append_ir2_opnd2i(LISA_ST_W, &temp, &latxs_env_ir2_opnd, offset);
        latxs_append_ir2_opnd2i(LISA_SRLI_D, &temp, &temp, 16);
        la_update_fp_status(temp);

        /* clear top */
        latxs_append_ir2_opnd2i(LISA_ST_W, &latxs_zero_ir2_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));

        /* set fptags */
        offset = lsenv_offset_of_tag_word(lsenv);
        lsassert(offset <= 0x7ff);
        li_d(temp, 0x101010101010101ULL);
        latxs_append_ir2_opnd2i(LISA_ST_D, &temp, &latxs_env_ir2_opnd, offset);
        offset = lsenv_offset_of_fpcs(lsenv);
        latxs_append_ir2_opnd2i(LISA_ST_W, &latxs_zero_ir2_opnd, &latxs_env_ir2_opnd, offset);
        latxs_append_ir2_opnd2i(LISA_ST_D, &latxs_zero_ir2_opnd, &latxs_env_ir2_opnd, offset + 4);
        latxs_append_ir2_opnd2i(LISA_ST_D, &latxs_zero_ir2_opnd, &latxs_env_ir2_opnd, offset + 12);

        latxs_ra_free_temp(&temp);

    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fninit, default_helper_cfg, HELPER_FNINIT);
    }
    return true;
}

bool latxs_translate_fldcw_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        IR2_OPND new_cw = latxs_ra_alloc_itemp();
        latxs_load_ir1_to_ir2(&new_cw, opnd, EXMode_Z);

        latxs_append_ir2_opnd2i(LISA_ST_H, &new_cw, &latxs_env_ir2_opnd, lsenv_offset_of_control_word(lsenv));
        la_update_fp_status(new_cw);
        latxs_ra_free_temp(&new_cw);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_load_ir1_to_ir2(&temp, opnd, EXMode_Z);
        latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg1_ir2_opnd, &temp);
        latxs_tr_gen_call_to_helper((ADDR)helper_fldcw, HELPER_FLDCW);
        latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
        latxs_ra_free_temp(&temp);
    }
    return true;
}

bool latxs_translate_fnstcw_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        /* 1. load the value of fpu control word */
        IR2_OPND cw_opnd = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_LD_HU, &cw_opnd, &latxs_env_ir2_opnd,
                                lsenv_offset_of_control_word(lsenv));

        /* 2. store the control word to the dest_opnd */
        /* fix capstone bug */
        latxs_store_ir2_to_ir1(&cw_opnd, ir1_get_opnd(pir1, 0));

        latxs_ra_free_temp(&cw_opnd);
    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fnstcw, default_helper_cfg, HELPER_FNSTCW);
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
        latxs_store_ir2_to_ir1(&temp, opnd);
        latxs_ra_free_temp(&temp);
    }

    return true;
}

bool latxs_translate_fnstsw_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
    opnd->size = 2;

    if (option_soft_fpu_fast) {
        IR2_OPND sw_opnd = latxs_ra_alloc_itemp();
        IR2_OPND top_opnd = latxs_ra_alloc_itemp();
        IR2_OPND tmp_opnd = latxs_ra_alloc_itemp();

        int status_offset = lsenv_offset_of_status_word(lsenv);
        lsassert(status_offset <= 0x7ff);
        latxs_append_ir2_opnd2i(LISA_LD_HU, &sw_opnd, &latxs_env_ir2_opnd, status_offset);

        latxs_append_ir2_opnd2i(LISA_LD_WU, &top_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &sw_opnd, &top_opnd, 13, 11);

        latxs_store_ir2_to_ir1(&sw_opnd, opnd);

        latxs_ra_free_temp(&sw_opnd);
        latxs_ra_free_temp(&top_opnd);
        latxs_ra_free_temp(&tmp_opnd);

    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fnstsw, default_helper_cfg, HELPER_FNSTSW);
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2_(lisa_mov, &temp, &latxs_ret0_ir2_opnd);
        latxs_store_ir2_to_ir1(&temp, opnd);
        latxs_ra_free_temp(&temp);
    }

    return true;
}

bool latxs_translate_fnstenv_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    int data_size = latxs_ir1_data_size(pir1);
    int data32 = data_size == 32 ? 1 : 0;

    if (option_soft_fpu_fast) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);

        IR2_OPND sw_cw_opnd = latxs_ra_alloc_itemp();
        IR2_OPND tag_opnd = latxs_ra_alloc_itemp();
        IR2_OPND tag_save = latxs_ra_alloc_itemp();
        IR2_OPND gpr_stn_low_opnd = latxs_ra_alloc_itemp();
        IR2_OPND gpr_stn_high_opnd = latxs_ra_alloc_itemp();
        IR2_OPND itemp = latxs_ra_alloc_itemp();

        /* update sw_opnd by tag */
        int offset = lsenv_offset_of_status_word(lsenv);
        lsassert(offset <= 0x7ff);
        latxs_append_ir2_opnd2i(LISA_LD_HU, &sw_cw_opnd, &latxs_env_ir2_opnd, offset);
        li_d(itemp, 0xc7ff);
        latxs_append_ir2_opnd3(LISA_AND, &sw_cw_opnd, &sw_cw_opnd, &itemp);

        latxs_append_ir2_opnd2i(LISA_LD_WU, &itemp, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));
        latxs_append_ir2_opnd2i(LISA_SLLI_W, &itemp, &itemp, 11);
        latxs_append_ir2_opnd3(LISA_OR, &sw_cw_opnd, &sw_cw_opnd, &itemp);
        latxs_append_ir2_opnd2i(LISA_SLLI_D, &sw_cw_opnd, &sw_cw_opnd, 16);

        offset = lsenv_offset_of_control_word(lsenv);
        lsassert(offset <= 0x7ff);
        latxs_append_ir2_opnd2i(LISA_LD_HU, &itemp, &latxs_env_ir2_opnd, offset);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &sw_cw_opnd, &itemp, 15, 0);

        offset = lsenv_offset_of_tag_word(lsenv);
        lsassert(offset <= 0x7ff);
        latxs_append_ir2_opnd2i(LISA_LD_D, &tag_opnd, &latxs_env_ir2_opnd, offset);

        li_d(tag_save, 0);
        for (int i = 7; i >= 0; i--) {
            IR2_OPND label_exit = latxs_ir2_opnd_new_label();
            IR2_OPND label_empty = latxs_ir2_opnd_new_label();
            IR2_OPND label_no_zero = latxs_ir2_opnd_new_label();
            IR2_OPND label_no_valid = latxs_ir2_opnd_new_label();

            latxs_append_ir2_opnd2i(LISA_SLLI_D, &tag_save, &tag_save, 2);
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &tag_opnd, 8 * (i + 1) - 1, 8 * i);
            latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_empty);

            offset = lsenv_offset_of_fpr(lsenv, i);
            lsassert(offset <= 0x7ff);
            latxs_append_ir2_opnd2i(LISA_LD_D, &gpr_stn_low_opnd, &latxs_env_ir2_opnd, offset);
            latxs_append_ir2_opnd2i(LISA_LD_HU, &gpr_stn_high_opnd, &latxs_env_ir2_opnd, offset + 8);


            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &gpr_stn_high_opnd, &latxs_zero_ir2_opnd, 15, 15);
            latxs_append_ir2_opnd3(LISA_BNE, &gpr_stn_high_opnd, &latxs_zero_ir2_opnd, &label_no_zero);
            latxs_append_ir2_opnd3(LISA_BNE, &gpr_stn_low_opnd, &latxs_zero_ir2_opnd, &label_no_zero);

            /* zero */
            latxs_append_ir2_opnd2i(LISA_ORI, &tag_save, &tag_save, 1);
            latxs_append_ir2_opnd1(LISA_B, &label_exit);

            latxs_append_ir2_opnd1(LISA_LABEL, &label_no_zero);
            latxs_append_ir2_opnd3(LISA_BEQ, &gpr_stn_high_opnd, &latxs_zero_ir2_opnd, &label_no_valid);
            li_wu(itemp, 0x7fff);
            latxs_append_ir2_opnd3(LISA_BEQ, &gpr_stn_high_opnd, &itemp, &label_no_valid);
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &gpr_stn_low_opnd, 63, 63);
            latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_no_valid);
            latxs_append_ir2_opnd1(LISA_B, &label_exit);

            latxs_append_ir2_opnd1(LISA_LABEL, &label_no_valid);
            latxs_append_ir2_opnd2i(LISA_ORI, &tag_save, &tag_save, 2);
            latxs_append_ir2_opnd1(LISA_B, &label_exit);

            latxs_append_ir2_opnd1(LISA_LABEL, &label_empty);
            latxs_append_ir2_opnd2i(LISA_ORI, &tag_save, &tag_save, 3);

            latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
        }
        latxs_ra_free_temp(&gpr_stn_low_opnd);
        latxs_ra_free_temp(&gpr_stn_high_opnd);
        latxs_ra_free_temp(&tag_opnd);
        latxs_ra_free_temp(&itemp);

        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

        if (data32) {
            itemp = latxs_ra_alloc_itemp();
            /* 32 bit */
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &sw_cw_opnd, 15, 0);
            latxs_append_ir2_opnd2i(LISA_SRLI_D, &sw_cw_opnd, &sw_cw_opnd, 16);
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &itemp, &sw_cw_opnd, 47, 32);
            gen_ldst_softmmu_helper(LISA_ST_D, &itemp, &mem_opnd, 0, 1);
            gen_ldst_softmmu_helper(LISA_ST_W, &tag_save, &mem_opnd, 8, 1);
            latxs_append_ir2_opnd2i(LISA_LD_W, &tag_save, &latxs_env_ir2_opnd, lsenv_offset_of_fpip(lsenv));
            gen_ldst_softmmu_helper(LISA_ST_W, &tag_save, &mem_opnd, 12, 1);
            latxs_append_ir2_opnd2i(LISA_LD_HU, &tag_save, &latxs_env_ir2_opnd, lsenv_offset_of_fpcs(lsenv));
            gen_ldst_softmmu_helper(LISA_ST_W, &tag_save, &mem_opnd, 16, 1);
            latxs_append_ir2_opnd2i(LISA_LD_W, &tag_save, &latxs_env_ir2_opnd, lsenv_offset_of_fpdp(lsenv));
            gen_ldst_softmmu_helper(LISA_ST_W, &tag_save, &mem_opnd, 20, 1);
            latxs_append_ir2_opnd2i(LISA_LD_HU, &tag_save, &latxs_env_ir2_opnd, lsenv_offset_of_fpds(lsenv));
            gen_ldst_softmmu_helper(LISA_ST_W, &tag_save, &mem_opnd, 24, 1);
        } else {
            /* 16 bit */
            gen_ldst_softmmu_helper(LISA_ST_W, &sw_cw_opnd, &mem_opnd, 0, 1);
            gen_ldst_softmmu_helper(LISA_ST_H, &tag_save, &mem_opnd, 4, 1);
            latxs_append_ir2_opnd2i(LISA_LD_H, &tag_save, &latxs_env_ir2_opnd, lsenv_offset_of_fpip(lsenv));
            gen_ldst_softmmu_helper(LISA_ST_H, &tag_save, &mem_opnd, 12, 1);
            latxs_append_ir2_opnd2i(LISA_LD_H, &tag_save, &latxs_env_ir2_opnd, lsenv_offset_of_fpcs(lsenv));
            gen_ldst_softmmu_helper(LISA_ST_H, &tag_save, &mem_opnd, 16, 1);
            latxs_append_ir2_opnd2i(LISA_LD_H, &tag_save, &latxs_env_ir2_opnd, lsenv_offset_of_fpdp(lsenv));
            gen_ldst_softmmu_helper(LISA_ST_H, &tag_save, &mem_opnd, 20, 1);
            latxs_append_ir2_opnd2i(LISA_LD_H, &tag_save, &latxs_env_ir2_opnd, lsenv_offset_of_fpds(lsenv));
            gen_ldst_softmmu_helper(LISA_ST_H, &tag_save, &mem_opnd, 24, 1);
        }
        latxs_ra_free_temp(&sw_cw_opnd);
        latxs_ra_free_temp(&tag_opnd);
        latxs_ra_free_temp(&tag_save);
        latxs_ra_free_temp(&itemp);
        latxs_ra_free_temp(&mem_opnd);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&temp, opnd);


        latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg2_ir2_opnd,
                                &latxs_zero_ir2_opnd, data32);
        latxs_tr_gen_call_to_helper((ADDR)helper_fstenv, HELPER_FSTENV);
        latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
    }

    return true;
}

bool latxs_translate_fldenv_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    int data_size = latxs_ir1_data_size(pir1);
    int data32 = data_size == 32 ? 1 : 0;

    if (option_soft_fpu_fast) {
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd0);

        IR2_OPND cw_opnd = latxs_ra_alloc_itemp();
        IR2_OPND sw_opnd = latxs_ra_alloc_itemp();
        IR2_OPND tag_opnd = latxs_ra_alloc_itemp();

        IR2_OPND label_exit = latxs_ir2_opnd_new_label();
        IR2_OPND label_restore_fpus = latxs_ir2_opnd_new_label();

        if (data32) {
            gen_ldst_softmmu_helper(LISA_LD_HU, &cw_opnd, &mem_opnd, 0, 1);
            gen_ldst_softmmu_helper(LISA_LD_HU, &sw_opnd, &mem_opnd, 4, 1);
            gen_ldst_softmmu_helper(LISA_LD_HU, &tag_opnd, &mem_opnd, 8, 1);
        } else {
            gen_ldst_softmmu_helper(LISA_LD_HU, &cw_opnd, &mem_opnd, 0, 1);
            gen_ldst_softmmu_helper(LISA_LD_HU, &sw_opnd, &mem_opnd, 2, 1);
            gen_ldst_softmmu_helper(LISA_LD_HU, &tag_opnd, &mem_opnd, 4, 1);
        }
        latxs_ra_free_temp(&mem_opnd);

        /* restore fpuc */
        latxs_append_ir2_opnd2i(LISA_ST_H, &cw_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_control_word(lsenv));
        la_update_fp_status(cw_opnd);

        IR2_OPND tag_save = latxs_ra_alloc_itemp();
        IR2_OPND itemp = latxs_ra_alloc_itemp();
        IR2_OPND itemp2 = latxs_ra_alloc_itemp();
        /* restore top */
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &sw_opnd, 13, 11);
        latxs_append_ir2_opnd2i(LISA_ST_W, &itemp, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));

        /* restore fpus */
        li_d(itemp, 0x47ff);
        latxs_append_ir2_opnd3(LISA_AND, &sw_opnd, &sw_opnd, &itemp);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &sw_opnd, 7, 7);
        latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_restore_fpus);

        /* set fpus busy*/
        li_d(itemp, 0x8000);
        latxs_append_ir2_opnd3(LISA_OR, &sw_opnd, &sw_opnd, &itemp);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_restore_fpus);
        li_d(itemp, 0x3f);
        latxs_append_ir2_opnd3(LISA_ANDN, &itemp, &itemp, &cw_opnd);
        latxs_append_ir2_opnd3(LISA_AND, &itemp, &sw_opnd, &itemp);
        latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_exit);
        li_d(itemp, 0x8080);
        latxs_append_ir2_opnd3(LISA_OR, &sw_opnd, &sw_opnd, &itemp);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);


        latxs_ra_free_temp(&cw_opnd);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
        latxs_append_ir2_opnd2i(LISA_ST_H, &sw_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_status_word(lsenv));

        li_wu(tag_save, 0);
        li_wu(itemp2, 0x3);

        for (int i = 7; i >= 0; i--) {
            IR2_OPND label_no_empty = latxs_ir2_opnd_new_label();
            latxs_append_ir2_opnd2i(LISA_SLLI_D, &tag_save, &tag_save, 8);
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &tag_opnd, 2 * i + 1, 2 * i);
            latxs_append_ir2_opnd3(LISA_BNE, &itemp, &itemp2, &label_no_empty);
            latxs_append_ir2_opnd2i(LISA_ORI, &tag_save, &tag_save, 0x1);

            latxs_append_ir2_opnd1(LISA_LABEL, &label_no_empty);
        }
        latxs_append_ir2_opnd2i(LISA_ST_D, &tag_save, &latxs_env_ir2_opnd, lsenv_offset_of_tag_word(lsenv));

        latxs_ra_free_temp(&sw_opnd);
        latxs_ra_free_temp(&tag_opnd);
        latxs_ra_free_temp(&tag_save);
        latxs_ra_free_temp(&itemp);
        latxs_ra_free_temp(&itemp2);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&temp, opnd);

        latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg2_ir2_opnd,
                                &latxs_zero_ir2_opnd, data32);
        latxs_tr_gen_call_to_helper((ADDR)helper_fldenv, HELPER_FLDENV);
        latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
    }

    return true;
}

bool latxs_translate_fnsave_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
#ifdef CONFIG_LATX_OPT_NOFP
    latxs_gen_nofp_sync_fprs_to_env();
#endif

    int data_size = latxs_ir1_data_size(pir1);
    int data32 = data_size == 32 ? 1 : 0;

    if (option_soft_fpu_fast) {
        latxs_translate_fnstenv_softfpu(pir1);

        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd);
        IR2_OPND gpr_stn_high_opnd = latxs_ra_alloc_itemp();
        IR2_OPND gpr_stn_low_opnd = latxs_ra_alloc_itemp();
        IR2_OPND top_opnd = latxs_ra_alloc_itemp();
        IR2_OPND addr_temp = latxs_ra_alloc_itemp();

        latxs_append_ir2_opnd2i(LISA_LD_WU, &top_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));

        /* store fpu data registers stack */
        int ptr = 14 << data32;
        int offset = lsenv_offset_of_fpr(lsenv, 0);

        for (int i = 0; i <= 7; i++) {
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &addr_temp, &top_opnd, i);
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &addr_temp, &addr_temp, 2, 0);
            latxs_append_ir2_opnd2i(LISA_SLLI_D, &addr_temp, &addr_temp, 4);
            latxs_append_ir2_opnd3(LISA_ADD_D, &addr_temp, &addr_temp, &latxs_env_ir2_opnd);
            latxs_append_ir2_opnd2i(LISA_LD_D, &gpr_stn_low_opnd, &addr_temp, offset);
            latxs_append_ir2_opnd2i(LISA_LD_HU, &gpr_stn_high_opnd, &addr_temp, offset + 8);

            gen_ldst_softmmu_helper(LISA_ST_D, &gpr_stn_low_opnd, &mem_opnd, ptr + 10 * i, 1);
            gen_ldst_softmmu_helper(LISA_ST_H, &gpr_stn_high_opnd, &mem_opnd, ptr + 10 * i + 8, 1);
        }

        latxs_ra_free_temp(&gpr_stn_low_opnd);
        latxs_ra_free_temp(&gpr_stn_high_opnd);
        latxs_ra_free_temp(&mem_opnd);
        latxs_ra_free_temp(&top_opnd);
        latxs_ra_free_temp(&addr_temp);
        latxs_translate_fninit_softfpu(pir1);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&temp, opnd);

        latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg2_ir2_opnd,
                                &latxs_zero_ir2_opnd, data32);
        latxs_tr_gen_call_to_helper((ADDR)helper_fsave, HELPER_FSAVE);
        latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
    }

    return true;
}

bool latxs_translate_frstor_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }

    int data_size = latxs_ir1_data_size(pir1);
    int data32 = data_size == 32 ? 1 : 0;

    if (option_soft_fpu_fast) {
        latxs_translate_fldenv_softfpu(pir1);

        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        IR2_OPND mem_opnd = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&mem_opnd, opnd);
        IR2_OPND gpr_stn_low_opnd = latxs_ra_alloc_itemp();
        IR2_OPND gpr_stn_high_opnd = latxs_ra_alloc_itemp();
        IR2_OPND top_opnd = latxs_ra_alloc_itemp();
        IR2_OPND addr_temp = latxs_ra_alloc_itemp();

        latxs_append_ir2_opnd2i(LISA_LD_WU, &top_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));

        int ptr = 14 << data32;
        int offset = lsenv_offset_of_fpr(lsenv, 0);
        for (int i = 0; i <= 7; i++) {
            gen_ldst_softmmu_helper(LISA_LD_D, &gpr_stn_low_opnd, &mem_opnd, ptr + 10 * i, 1);
            gen_ldst_softmmu_helper(LISA_LD_HU, &gpr_stn_high_opnd, &mem_opnd, ptr + 10 * i + 8, 1);

            latxs_append_ir2_opnd2i(LISA_ADDI_D, &addr_temp, &top_opnd, i);
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &addr_temp, &addr_temp, 2, 0);
            latxs_append_ir2_opnd2i(LISA_SLLI_D, &addr_temp, &addr_temp, 4);
            latxs_append_ir2_opnd3(LISA_ADD_D, &addr_temp, &addr_temp, &latxs_env_ir2_opnd);
            latxs_append_ir2_opnd2i(LISA_ST_D, &gpr_stn_low_opnd, &addr_temp, offset);
            latxs_append_ir2_opnd2i(LISA_ST_H, &gpr_stn_high_opnd, &addr_temp, offset + 8);
        }
        latxs_ra_free_temp(&gpr_stn_low_opnd);
        latxs_ra_free_temp(&gpr_stn_high_opnd);
        latxs_ra_free_temp(&mem_opnd);
        latxs_ra_free_temp(&top_opnd);
        latxs_ra_free_temp(&addr_temp);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        IR2_OPND temp = latxs_ra_alloc_itemp();
        latxs_convert_mem_opnd_no_offset(&temp, opnd);

        latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg2_ir2_opnd,
                                &latxs_zero_ir2_opnd, data32);
        latxs_tr_gen_call_to_helper((ADDR)helper_frstor, HELPER_FRSTOR);
        latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
    }

#ifdef CONFIG_LATX_OPT_NOFP
    latxs_gen_nofp_sync_fprs_from_env();
#endif

    return true;
}

bool latxs_translate_fnclex_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND sw_value = latxs_ra_alloc_itemp();

        latxs_append_ir2_opnd2i(LISA_LD_HU, &sw_value, &latxs_env_ir2_opnd, lsenv_offset_of_status_word(lsenv));
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &sw_value, &sw_value, 14, 8);
        latxs_append_ir2_opnd2i(LISA_SLLI_D, &sw_value, &sw_value, 8);
        latxs_append_ir2_opnd2i(LISA_ST_H, &sw_value, &latxs_env_ir2_opnd, lsenv_offset_of_status_word(lsenv));

        latxs_ra_free_temp(&sw_value);

    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fclex, nofp_helper_cfg, HELPER_FCLEX);
    }
    return true;
}

bool latxs_translate_fincstp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND top_opnd = latxs_ra_alloc_itemp();

        latxs_append_ir2_opnd2i(LISA_LD_W, &top_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &top_opnd, &top_opnd, 1);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &top_opnd, &top_opnd, 2, 0);
        latxs_append_ir2_opnd2i(LISA_ST_W, &top_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));

        /* increments the top-of-stack pointer */
        IR2_OPND value_status = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_LD_H, &value_status, &latxs_env_ir2_opnd,
                          lsenv_offset_of_status_word(lsenv)); /* status_word */

        /* set C3 C2 C1 C0 flag to 0 */
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &value_status, &latxs_zero_ir2_opnd, 10, 8);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &value_status, &latxs_zero_ir2_opnd, 14, 14);
        latxs_append_ir2_opnd2i(LISA_ST_H, &value_status, &latxs_env_ir2_opnd,
                          lsenv_offset_of_status_word(lsenv)); /* status_word */

        latxs_ra_free_temp(&top_opnd);
        latxs_ra_free_temp(&value_status);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fincstp, nofp_helper_cfg, HELPER_FINCSTP);
    }
    return true;
}

bool latxs_translate_fdecstp_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        IR2_OPND top_opnd = latxs_ra_alloc_itemp();

        latxs_append_ir2_opnd2i(LISA_LD_W, &top_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &top_opnd, &top_opnd, -1);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &top_opnd, &top_opnd, 2, 0);
        latxs_append_ir2_opnd2i(LISA_ST_W, &top_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));

        /* increments the top-of-stack pointer */
        IR2_OPND value_status = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_LD_H, &value_status, &latxs_env_ir2_opnd,
                          lsenv_offset_of_status_word(lsenv)); /* status_word */

        /* set C3 C2 C1 C0 flag to 0 */
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &value_status, &latxs_zero_ir2_opnd, 10, 8);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &value_status, &latxs_zero_ir2_opnd, 14, 14);
        latxs_append_ir2_opnd2i(LISA_ST_H, &value_status, &latxs_env_ir2_opnd,
                          lsenv_offset_of_status_word(lsenv)); /* status_word */
        latxs_ra_free_temp(&top_opnd);
        latxs_ra_free_temp(&value_status);

    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fdecstp, nofp_helper_cfg, HELPER_FDECSTP);
    }
    return true;
}

bool latxs_translate_ffree_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        /*
         * 1. get real current top
         *    The td->curr_top might be rotated along with the FPRs.
         *    But the tag word is not rotated.
         *    It should be fixed with top_bias.
         */
        IR2_OPND real_top = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_LD_WU, &real_top, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));

        /* 2. tag_offset = (real_top + reg) & 0x7 */
        IR2_OPND tag_offset_opnd = latxs_ra_alloc_itemp();
        int reg = ir1_opnd_base_reg_num(ir1_get_opnd(pir1, 0));
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &tag_offset_opnd,
                                &real_top, reg);
        latxs_append_ir2_opnd2i(LISA_ANDI, &tag_offset_opnd,
                                &tag_offset_opnd, 0x7);
        /* 3. address of tag = env + tag_offset. */
        IR2_OPND address = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &address,
                                &latxs_env_ir2_opnd,
                                lsenv_offset_of_tag_word(lsenv));
        latxs_append_ir2_opnd3(LISA_ADD_D, &address,
                               &address, &tag_offset_opnd);
        /* 4. set tag = 1 */
        IR2_OPND tag = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_ORI,  &tag, &latxs_zero_ir2_opnd, 1);
        latxs_append_ir2_opnd2i(LISA_ST_B, &tag, &address,       0);

        latxs_ra_free_temp(&real_top);
        latxs_ra_free_temp(&tag_offset_opnd);
        latxs_ra_free_temp(&address);
        latxs_ra_free_temp(&tag);

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        int reg_num = ir1_opnd_base_reg_num(opnd);
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_ffree_STN, reg_num,
                                         nofp_helper_cfg, HELPER_FFREE_STN);
    }
    return true;
}

bool latxs_translate_ffreep_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    if (option_soft_fpu_fast) {
        latxs_translate_ffree_softfpu(pir1);
        la_fpu_pop();

    } else {
        IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
        int reg_num = ir1_opnd_base_reg_num(opnd);
        latxs_tr_gen_call_to_helper2_cfg((ADDR)helper_ffree_STN, reg_num,
                                         nofp_helper_cfg, HELPER_FFREE_STN);
        la_fpu_pop();
    }
    return true;
}

bool latxs_translate_fnop_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    int status_offset = lsenv_offset_of_status_word(lsenv);
    IR2_OPND fpus = latxs_ra_alloc_itemp();
    IR2_OPND no_excp = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd2i(LISA_LD_H, &fpus, &latxs_env_ir2_opnd, status_offset);
    latxs_append_ir2_opnd2i(LISA_ANDI, &fpus, &fpus, 1 << 7); //FPU_SE
    latxs_append_ir2_opnd3(LISA_BEQ, &fpus, &latxs_zero_ir2_opnd, &no_excp);
    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fwait, nofp_helper_cfg,
                HELPER_FWAIT);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fwait, default_helper_cfg,
                HELPER_FWAIT);
    }
    latxs_append_ir2_opnd1(LISA_LABEL, &no_excp);
    return true;
}

bool latxs_translate_wait_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_fp_common_excp_check(pir1)) {
        return true;
    }
    int status_offset = lsenv_offset_of_status_word(lsenv);
    IR2_OPND fpus = latxs_ra_alloc_itemp();
    IR2_OPND no_excp = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd2i(LISA_LD_H, &fpus, &latxs_env_ir2_opnd, status_offset);
    latxs_append_ir2_opnd2i(LISA_ANDI, &fpus, &fpus, 1 << 7); //FPU_SE
    latxs_append_ir2_opnd3(LISA_BEQ, &fpus, &latxs_zero_ir2_opnd, &no_excp);
    if (option_soft_fpu_fast) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fwait, nofp_helper_cfg,
                HELPER_FWAIT);
    } else {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_fwait, default_helper_cfg,
                HELPER_FWAIT);
    }
    latxs_append_ir2_opnd1(LISA_LABEL, &no_excp);
    return true;
}

/* non-x87 instruction, but accesss fpu stack */
bool latxs_translate_fxsave_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_excp_check(pir1)) {
        return true;
    }
#ifdef CONFIG_LATX_OPT_NOFP
    latxs_gen_nofp_sync_fprs_to_env();
#endif

    /* update mxcsr flags from fcsr */
    {
        IR2_OPND mxcsr_opnd = latxs_ra_alloc_itemp();
        IR2_OPND fcsr = latxs_ra_alloc_itemp();
        IR2_OPND temp1 = latxs_ra_alloc_itemp();

        latxs_append_ir2_opnd2i(LISA_LD_WU, &mxcsr_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_mxcsr(lsenv));
        latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &fcsr, &latxs_fcsr2_ir2_opnd);
        /* 19:16[zoup] -> 7:4[zoup] -> 3:0[puoz] -> 5:2[puoz] */
        latxs_append_ir2_opnd2i(LISA_SRLI_W, &temp1, &fcsr, 12);
        latxs_append_ir2_opnd2(LISA_BITREV_4B, &temp1, &temp1);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &mxcsr_opnd, &temp1, 5, 2);
        /* IE */
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp1, &fcsr, 20, 20);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &mxcsr_opnd, &temp1, 0, 0);
        latxs_append_ir2_opnd2i(LISA_ST_W, &mxcsr_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_mxcsr(lsenv));

        latxs_ra_free_temp(&fcsr);
        latxs_ra_free_temp(&temp1);
        latxs_ra_free_temp(&mxcsr_opnd);
    }
    IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
    IR2_OPND temp = latxs_ra_alloc_itemp();
    latxs_convert_mem_opnd_no_offset(&temp, opnd);

    if (option_soft_fpu_fast) {
        IR2_OPND mem_opnd = temp;
        IR2_OPND itemp = latxs_ra_alloc_itemp();
        IR2_OPND label_helper = latxs_ir2_opnd_new_label();
        IR2_OPND label_exit = latxs_ir2_opnd_new_label();

        //latxs_append_ir2_opnd1(LISA_B, &label_helper);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &temp, 3, 0);
        latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_helper);

        /* do_xsave_fpu */
        {
            IR2_OPND sw_cw_opnd = latxs_ra_alloc_itemp();
            IR2_OPND tag_opnd = latxs_ra_alloc_itemp();
            IR2_OPND tag_save = latxs_ra_alloc_itemp();

            /* update sw_opnd by tag */
            int offset = lsenv_offset_of_status_word(lsenv);
            lsassert(offset <= 0x7ff);
            latxs_append_ir2_opnd2i(LISA_LD_HU, &sw_cw_opnd, &latxs_env_ir2_opnd, offset);
            latxs_append_ir2_opnd2i(LISA_LD_WU, &itemp, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &sw_cw_opnd, &itemp, 13, 11);
            latxs_append_ir2_opnd2i(LISA_SLLI_D, &sw_cw_opnd, &sw_cw_opnd, 16);

            offset = lsenv_offset_of_control_word(lsenv);
            lsassert(offset <= 0x7ff);
            latxs_append_ir2_opnd2i(LISA_LD_HU, &itemp, &latxs_env_ir2_opnd, offset);
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &sw_cw_opnd, &itemp, 15, 0);

            offset = lsenv_offset_of_tag_word(lsenv);
            lsassert(offset <= 0x7ff);
            latxs_append_ir2_opnd2i(LISA_LD_D, &tag_opnd, &latxs_env_ir2_opnd, offset);

            li_d(tag_save, 0);
            for (int i = 7; i >= 0; i--) {
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &tag_opnd, 8 * (i + 1) - 1, 8 * i);
                latxs_append_ir2_opnd2i(LISA_XORI, &itemp, &itemp, 0x1);
                latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &tag_save, &itemp, i, i);
            }
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &sw_cw_opnd, &tag_save, 63, 32);
            gen_ldst_softmmu_helper(LISA_ST_D, &sw_cw_opnd, &mem_opnd, 0, 1);
            gen_ldst_softmmu_helper(LISA_ST_D, &latxs_zero_ir2_opnd, &mem_opnd, 8, 1);
            gen_ldst_softmmu_helper(LISA_ST_D, &latxs_zero_ir2_opnd, &mem_opnd, 16, 1);

            latxs_ra_free_temp(&tag_opnd);
            latxs_ra_free_temp(&tag_save);
            latxs_ra_free_temp(&sw_cw_opnd);

            IR2_OPND gpr_stn_low_opnd = latxs_ra_alloc_itemp();
            IR2_OPND gpr_stn_high_opnd = latxs_ra_alloc_itemp();
            IR2_OPND addr_temp = latxs_ra_alloc_itemp();

            latxs_append_ir2_opnd2i(LISA_LD_WU, &itemp, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));

            /* store fpu data registers stack */
            int fpu_offset = lsenv_offset_of_fpr(lsenv, 0);

            for (int i = 0; i <= 7; i++) {
                latxs_append_ir2_opnd2i(LISA_ADDI_D, &addr_temp, &itemp, i);
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &addr_temp, &addr_temp, 2, 0);
                latxs_append_ir2_opnd2i(LISA_SLLI_D, &addr_temp, &addr_temp, 4);
                latxs_append_ir2_opnd3(LISA_ADD_D, &addr_temp, &addr_temp, &latxs_env_ir2_opnd);
                latxs_append_ir2_opnd2i(LISA_LD_D, &gpr_stn_low_opnd, &addr_temp, fpu_offset);
                latxs_append_ir2_opnd2i(LISA_LD_HU, &gpr_stn_high_opnd, &addr_temp, fpu_offset + 8);

                gen_ldst_softmmu_helper(LISA_ST_D, &gpr_stn_low_opnd, &mem_opnd, 32 + 16 * i, 1);
                gen_ldst_softmmu_helper(LISA_ST_H, &gpr_stn_high_opnd, &mem_opnd, 32 + 16 * i + 8, 1);
            }
            latxs_ra_free_temp(&gpr_stn_low_opnd);
            latxs_ra_free_temp(&gpr_stn_high_opnd);
            latxs_ra_free_temp(&addr_temp);
        }

        /* do_xsave_mxcsr */
        {
            IR2_OPND mxcsr_opnd = latxs_ra_alloc_itemp();
            IR2_OPND label_exit_sse = latxs_ir2_opnd_new_label();

            /* env->cr[4] & CR4_OSFXSR_MASK */
            latxs_append_ir2_opnd2i(LISA_LD_WU, &itemp, &latxs_env_ir2_opnd, lsenv_offset_of_cr(lsenv, 4));
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 9, 9);
            latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_exit_sse);

            /* do_xsave_mxcsr */
            latxs_append_ir2_opnd2i(LISA_LD_WU, &mxcsr_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_mxcsr(lsenv));
            li_d(itemp, 0xffff);
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &mxcsr_opnd, &itemp, 63, 32);
            gen_ldst_softmmu_helper(LISA_ST_D, &mxcsr_opnd, &mem_opnd, 24, 1);

            latxs_ra_free_temp(&mxcsr_opnd);
            /* do_xsave_sse */
            {
                IR2_OPND hflags = latxs_ra_alloc_itemp();
                IR2_OPND ftemp = latxs_ra_alloc_ftemp();
                IR2_OPND num = latxs_ra_alloc_itemp();
                IR2_OPND label_save_sse = latxs_ir2_opnd_new_label();

                li_d(num, 8);
                /* !(env->efer & MSR_EFER_FFXSR */
                latxs_append_ir2_opnd2i(LISA_LD_D, &itemp, &latxs_env_ir2_opnd, lsenv_offset_of_efer(lsenv));
                latxs_append_ir2_opnd2i(LISA_LD_WU, &hflags, &latxs_env_ir2_opnd, lsenv_offset_of_hflags(lsenv));
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 14, 14);
                latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_save_sse);

                /* (env->hflags & HF_CPL_MASK) || !(env->hflags & HF_LMA_MASK) */
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &hflags, 1, 0);
                latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_save_sse);
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &hflags, 14, 14);
                latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_exit_sse);

                latxs_append_ir2_opnd1(LISA_LABEL, &label_save_sse);
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &hflags, &hflags, 15, 15);
                latxs_append_ir2_opnd3(LISA_SLL_D, &num, &num, &hflags);

                for (int i = 0; i < CPU_NB_REGS; ++i) {
                    IR2_OPND xmm_opnd = latxs_ra_alloc_xmm(i);
                    if (!option_fast_fpr_ldst) {
                        gen_ldst_softmmu_helper(LISA_VST, &xmm_opnd, &mem_opnd, 160 + i * 16, 1);
                    } else {
                        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &xmm_opnd);
                        gen_ldst_softmmu_helper(LISA_ST_D, &itemp, &mem_opnd, 160 + i * 16, 1);
                        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &ftemp, &xmm_opnd, 0x1);
                        latxs_append_ir2_opnd2(LISA_MOVFR2GR_D, &itemp, &ftemp);
                        gen_ldst_softmmu_helper(LISA_ST_D, &itemp, &mem_opnd, 160 + i * 16 + 8, 1);
                    }
                    latxs_append_ir2_opnd2i(LISA_ADDI_D, &num, &num, -1);
                    latxs_append_ir2_opnd3(LISA_BEQ, &num, &latxs_zero_ir2_opnd, &label_exit_sse);
                }
                latxs_ra_free_temp(&hflags);
                latxs_ra_free_temp(&num);
                latxs_ra_free_temp(&ftemp);
            }
            latxs_append_ir2_opnd1(LISA_LABEL, &label_exit_sse);
        }
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_helper);
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_tr_gen_call_to_helper((ADDR)helper_fxsave, HELPER_FXSAVE);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    } else {
        latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_tr_gen_call_to_helper((ADDR)helper_fxsave, HELPER_FXSAVE);
        latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
    }
    return true;
}

/* non-x87 instruction, but accesss fpu stack */
bool latxs_translate_fxrstor_softfpu(IR1_INST *pir1)
{
    if (latxs_tr_gen_excp_check(pir1)) {
        return true;
    }

    IR1_OPND *opnd = ir1_get_opnd(pir1, 0);
    IR2_OPND temp = latxs_ra_alloc_itemp();
    latxs_convert_mem_opnd_no_offset(&temp, opnd);

    if (option_soft_fpu_fast) {
        IR2_OPND mem_opnd = temp;
        IR2_OPND itemp = latxs_ra_alloc_itemp();
        IR2_OPND label_helper = latxs_ir2_opnd_new_label();
        IR2_OPND label_exit = latxs_ir2_opnd_new_label();
        IR2_OPND label_mxcsr_valid = latxs_ir2_opnd_new_label();

        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &temp, 3, 0);
        latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_helper);

        /* Validate MXCSR before restoring any architectural state. */
        latxs_append_ir2_opnd2i(LISA_LD_D, &itemp, &latxs_env_ir2_opnd,
                                lsenv_offset_of_cr(lsenv, 4));
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 9, 9);
        latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd,
                               &label_mxcsr_valid);
        gen_ldst_softmmu_helper(LISA_LD_WU, &itemp, &mem_opnd, 24, 1);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 31, 16);
        latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd,
                               &label_mxcsr_valid);
        latxs_tr_gen_excp_gp(pir1, 0);
        latxs_append_ir2_opnd1(LISA_LABEL, &label_mxcsr_valid);

        /* do_xrstor_fpu */
        {
            IR2_OPND sw_opnd = latxs_ra_alloc_itemp();
            IR2_OPND cw_opnd = latxs_ra_alloc_itemp();
            IR2_OPND tag_opnd = latxs_ra_alloc_itemp();
            IR2_OPND label_fpu_exit = latxs_ir2_opnd_new_label();
            IR2_OPND label_restore_fpus = latxs_ir2_opnd_new_label();

            gen_ldst_softmmu_helper(LISA_LD_D, &itemp, &mem_opnd, 0, 1);
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &cw_opnd, &itemp, 15, 0);
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &sw_opnd, &itemp, 31, 16);
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &tag_opnd, &itemp, 39, 32);

            /* restore fpuc */
            latxs_append_ir2_opnd2i(LISA_ST_H, &cw_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_control_word(lsenv));

            /* restore top */
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &sw_opnd, 13, 11);
            latxs_append_ir2_opnd2i(LISA_ST_W, &itemp, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));

            /* restore fpus */
            li_d(itemp, 0x47ff);
            latxs_append_ir2_opnd3(LISA_AND, &sw_opnd, &sw_opnd, &itemp);
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &sw_opnd, 7, 7);
            latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_restore_fpus);

            /* set fpus busy*/
            li_d(itemp, 0x8000);
            latxs_append_ir2_opnd3(LISA_OR, &sw_opnd, &sw_opnd, &itemp);

            latxs_append_ir2_opnd1(LISA_LABEL, &label_restore_fpus);
            li_d(itemp, 0x3f);
            latxs_append_ir2_opnd3(LISA_ANDN, &itemp, &itemp, &cw_opnd);
            latxs_append_ir2_opnd3(LISA_AND, &itemp, &sw_opnd, &itemp);
            latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_fpu_exit);
            li_d(itemp, 0x8080);
            latxs_append_ir2_opnd3(LISA_OR, &sw_opnd, &sw_opnd, &itemp);
            latxs_append_ir2_opnd1(LISA_B, &label_fpu_exit);


            latxs_append_ir2_opnd1(LISA_LABEL, &label_fpu_exit);
            latxs_append_ir2_opnd2i(LISA_ST_H, &sw_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_status_word(lsenv));

            latxs_ra_free_temp(&sw_opnd);
            /* update_fp_status */
            {
                IR2_OPND itemp2 = latxs_ra_alloc_itemp();

                IR2_OPND label_float64_float80 = latxs_ir2_opnd_new_label();

                int fp_status_offset = lsenv_offset_of_fp_status(lsenv);
                int round_mode_offset = fp_status_offset +
                    offsetof(float_status, float_rounding_mode);
                int round_precision_offset = fp_status_offset +
                    offsetof(float_status, floatx80_rounding_precision);

                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp2, &cw_opnd, 11, 10);
                latxs_append_ir2_opnd2i(LISA_ST_B, &itemp2, &latxs_env_ir2_opnd, round_mode_offset);

                /*
                 *turn x86 pc to env pc
                 *          x86      env
                 *
                 * 32       00       10
                 * 64       10       01
                 * 80       11       00
                 *
                 */

                /* Precision Control (9, 8)*/
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp2, &cw_opnd, 9, 8);
                latxs_append_ir2_opnd3(LISA_BNE, &itemp2, &latxs_zero_ir2_opnd, &label_float64_float80);
                li_wu(itemp2, 1);

                latxs_append_ir2_opnd1(LISA_LABEL, &label_float64_float80);
                latxs_append_ir2_opnd2i(LISA_XORI, &itemp2, &itemp2, 3);
                latxs_append_ir2_opnd2i(LISA_ST_B, &itemp2, &latxs_env_ir2_opnd, round_precision_offset);

                latxs_ra_free_temp(&itemp2);
            }
            latxs_ra_free_temp(&cw_opnd);

            IR2_OPND tag_save = latxs_ra_alloc_itemp();
            li_d(tag_save, 0);

            latxs_append_ir2_opnd2i(LISA_XORI, &tag_opnd, &tag_opnd, 0xff);
            for (int i = 7; i >= 0; i--) {
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &tag_opnd, i, i);
                latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &tag_save, &itemp, 8 * (i + 1) - 1, 8 * i);
            }
            latxs_append_ir2_opnd2i(LISA_ST_D, &tag_save, &latxs_env_ir2_opnd, lsenv_offset_of_tag_word(lsenv));

            latxs_ra_free_temp(&tag_save);
            latxs_ra_free_temp(&tag_opnd);

            IR2_OPND gpr_stn_low_opnd = latxs_ra_alloc_itemp();
            IR2_OPND gpr_stn_high_opnd = latxs_ra_alloc_itemp();
            IR2_OPND addr_temp = latxs_ra_alloc_itemp();

            latxs_append_ir2_opnd2i(LISA_LD_WU, &itemp, &latxs_env_ir2_opnd, lsenv_offset_of_top(lsenv));

            /* store fpu data registers stack */
            int fpu_offset = lsenv_offset_of_fpr(lsenv, 0);

            for (int i = 0; i <= 7; i++) {
                gen_ldst_softmmu_helper(LISA_LD_D, &gpr_stn_low_opnd, &mem_opnd, 32 + 16 * i, 1);
                gen_ldst_softmmu_helper(LISA_LD_HU, &gpr_stn_high_opnd, &mem_opnd, 32 + 16 * i + 8, 1);

                latxs_append_ir2_opnd2i(LISA_ADDI_D, &addr_temp, &itemp, i);
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &addr_temp, &addr_temp, 2, 0);
                latxs_append_ir2_opnd2i(LISA_SLLI_D, &addr_temp, &addr_temp, 4);
                latxs_append_ir2_opnd3(LISA_ADD_D, &addr_temp, &addr_temp, &latxs_env_ir2_opnd);
                latxs_append_ir2_opnd2i(LISA_ST_D, &gpr_stn_low_opnd, &addr_temp, fpu_offset);
                latxs_append_ir2_opnd2i(LISA_ST_H, &gpr_stn_high_opnd, &addr_temp, fpu_offset + 8);
            }
            latxs_ra_free_temp(&gpr_stn_low_opnd);
            latxs_ra_free_temp(&gpr_stn_high_opnd);
            latxs_ra_free_temp(&addr_temp);
        }

        IR2_OPND hflags = latxs_ra_alloc_itemp();
        /* do_xrstor_mxcsr */
        {
            IR2_OPND mxcsr_opnd = latxs_ra_alloc_itemp();
            IR2_OPND label_exit_sse = latxs_ir2_opnd_new_label();

            /* env->cr[4] & CR4_OSFXSR_MASK */
            latxs_append_ir2_opnd2i(LISA_LD_WU, &itemp, &latxs_env_ir2_opnd, lsenv_offset_of_cr(lsenv, 4));
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 9, 9);
            latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_exit_sse);

            /* do_xrstor_mxcsr */
            gen_ldst_softmmu_helper(LISA_LD_WU, &mxcsr_opnd, &mem_opnd, 24, 1);
            latxs_append_ir2_opnd2i(LISA_ST_W, &mxcsr_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_mxcsr(lsenv));

            /* update_mxcsr_status */
            {
#if 0
// sse is emulated with lsx insns, sse status not needed
                /* set_x86_rounding_mode */
                int sse_status_offset = lsenv_offset_of_sse_status(lsenv);
                int sse_round_mode_offset = sse_status_offset +
                    offsetof(float_status, float_rounding_mode);

                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &mxcsr_opnd, 14, 13);
                latxs_append_ir2_opnd2i(LISA_ST_B, &itemp, &latxs_env_ir2_opnd, sse_round_mode_offset);

                /* set_float_exception_flags */
                int sse_float_exception_flags = sse_status_offset +
                    offsetof(float_status, float_exception_flags);
                latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &itemp, &mxcsr_opnd, 5, 2);
                latxs_append_ir2_opnd2i(LISA_SLLI_D, &itemp, &itemp, 1);
                latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &itemp, &mxcsr_opnd, 0, 0);
                latxs_append_ir2_opnd2i(LISA_ST_H, &itemp, &latxs_env_ir2_opnd, sse_float_exception_flags);


                /* set_flush_inputs_to_zero */
                int sse_flush_inputs_to_zero = sse_status_offset +
                    offsetof(float_status, float_exception_flags);
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &mxcsr_opnd, 6, 6);
                latxs_append_ir2_opnd2i(LISA_ST_B, &itemp, &latxs_env_ir2_opnd, sse_flush_inputs_to_zero);

                /* set_flush_to_zero */
                int sse_flush_to_zero = sse_status_offset +
                    offsetof(float_status, float_exception_flags);
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &mxcsr_opnd, 15, 15);
                latxs_append_ir2_opnd2i(LISA_ST_B, &itemp, &latxs_env_ir2_opnd, sse_flush_to_zero);
#endif

                IR2_OPND number_1 = latxs_ra_alloc_itemp();

                /* HF_SIGFPE_ON_MASK */
                li_d(number_1, 1);
                latxs_append_ir2_opnd2i(LISA_LD_WU, &hflags, &latxs_env_ir2_opnd, lsenv_offset_of_hflags(lsenv));
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &mxcsr_opnd, 12, 7);
                latxs_append_ir2_opnd2i(LISA_XORI, &itemp, &itemp, 0x3f);
                latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &itemp, &latxs_zero_ir2_opnd, 1, 1);
                latxs_append_ir2_opnd3(LISA_MASKEQZ, &itemp, &number_1, &itemp);
                latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &hflags, &itemp, HF_SIGFPE_ON, HF_SIGFPE_ON);
                latxs_append_ir2_opnd2i(LISA_ST_W, &hflags,
                        &latxs_env_ir2_opnd, lsenv_offset_of_hflags(lsenv));

                latxs_ra_free_temp(&number_1);
            }

            latxs_ra_free_temp(&mxcsr_opnd);
            /* do_xrstor_sse */
            {
                IR2_OPND ftemp = latxs_ra_alloc_ftemp();
                IR2_OPND num = latxs_ra_alloc_itemp();
                IR2_OPND label_xrstor_sse = latxs_ir2_opnd_new_label();

                li_d(num, 8);
                /* !(env->efer & MSR_EFER_FFXSR */
                latxs_append_ir2_opnd2i(LISA_LD_D, &itemp, &latxs_env_ir2_opnd, lsenv_offset_of_efer(lsenv));
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &itemp, 14, 14);
                latxs_append_ir2_opnd3(LISA_BEQ, &itemp, &latxs_zero_ir2_opnd, &label_xrstor_sse);

                /* (env->hflags & HF_CPL_MASK) || !(env->hflags & HF_LMA_MASK) */
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &hflags, 1, 0);
                latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_xrstor_sse);
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &itemp, &hflags, 14, 14);
                latxs_append_ir2_opnd3(LISA_BNE, &itemp, &latxs_zero_ir2_opnd, &label_exit_sse);

                latxs_append_ir2_opnd1(LISA_LABEL, &label_xrstor_sse);
                latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &hflags, &hflags, 15, 15);
                latxs_append_ir2_opnd3(LISA_SLL_D, &num, &num, &hflags);

                for (int i = 0; i < CPU_NB_REGS; ++i) {
                    IR2_OPND xmm_opnd = latxs_ra_alloc_xmm(i);
                    if (option_fast_fpr_ldst) {
                        gen_ldst_softmmu_helper(LISA_VLD, &xmm_opnd, &mem_opnd, 160 + i * 16, 1);
                    } else {
                        gen_ldst_softmmu_helper(LISA_LD_D, &itemp, &mem_opnd, 160 + i * 16, 1);
                        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &xmm_opnd, &itemp);
                        gen_ldst_softmmu_helper(LISA_LD_D, &itemp, &mem_opnd, 160 + i * 16 + 8, 1);
                        latxs_append_ir2_opnd2(LISA_MOVGR2FR_D, &ftemp, &itemp);
                        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &xmm_opnd, &ftemp, 0x10);
                    }
                    latxs_append_ir2_opnd2i(LISA_ADDI_D, &num, &num, -1);
                    latxs_append_ir2_opnd3(LISA_BEQ, &num, &latxs_zero_ir2_opnd, &label_exit_sse);
                }
                latxs_ra_free_temp(&num);
                latxs_ra_free_temp(&ftemp);
            }
            latxs_append_ir2_opnd1(LISA_LABEL, &label_exit_sse);
        }
        latxs_ra_free_temp(&hflags);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_helper);
        latxs_tr_gen_call_to_helper_prologue_cfg(nofp_helper_cfg);
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_tr_gen_call_to_helper((ADDR)helper_fxrstor, HELPER_FXRSTOR);
        latxs_tr_gen_call_to_helper_epilogue_cfg(nofp_helper_cfg);
        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    } else {
        latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
        LATXS_MOV_ADDR_TO_ARG(latxs_arg1_ir2_opnd, temp);
        latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                &latxs_env_ir2_opnd);
        if (hamt_enabled()) {
            latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        }
        latxs_tr_gen_call_to_helper((ADDR)helper_fxrstor, HELPER_FXRSTOR);
        latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
    }

#ifdef CONFIG_LATX_OPT_NOFP
    latxs_gen_nofp_sync_fprs_from_env();
#endif

    if (option_lazy_mxcsr) {
        IR2_OPND invalid_mxcsr = latxs_ra_alloc_itemp();
        uint32_t old_mxcsr_offset = lsenv_offset_of_old_mxcsr(lsenv);

        lsassert(old_mxcsr_offset <= 0x7ff);
        latxs_load_imm32_to_ir2(&invalid_mxcsr, UINT32_MAX, EXMode_Z);
        latxs_append_ir2_opnd2i(LISA_ST_W, &invalid_mxcsr,
                &latxs_env_ir2_opnd, old_mxcsr_offset);
        latxs_ra_free_temp(&invalid_mxcsr);
    }

#if 1
// sse is emulated with lsx insns, upate fcsrs
    {
        IR2_OPND new_mxcsr = latxs_ra_alloc_itemp();
        IR2_OPND temp = latxs_ra_alloc_itemp();
        IR2_OPND temp1 = latxs_ra_alloc_itemp();
        IR2_OPND temp2 = latxs_ra_alloc_itemp();
        IR2_OPND label2 = latxs_ir2_opnd_new_label();
        latxs_append_ir2_opnd2i(LISA_LD_W, &new_mxcsr, &latxs_env_ir2_opnd, lsenv_offset_of_mxcsr(lsenv));
        if (option_lazy_mxcsr) {
            uint32 offset1 = lsenv_offset_of_old_mxcsr(lsenv);
            lsassert(offset1 <= 0x7ff);
            latxs_append_ir2_opnd2i(LISA_LD_W, &temp,
                    &latxs_env_ir2_opnd, offset1);
            latxs_append_ir2_opnd2i(LISA_ST_W, &new_mxcsr,
                    &latxs_env_ir2_opnd, offset1);
            latxs_append_ir2_opnd3(LISA_BEQ, &temp, &new_mxcsr, &label2);
        }

        /* rounding */
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp, &new_mxcsr, 14, 13);
        latxs_append_ir2_opnd2i(LISA_ANDI, &temp1, &temp, 0x1);
        latxs_append_ir2_opnd2i(LISA_XORI, &temp2, &temp, 0x2);
        latxs_append_ir2_opnd3(LISA_MASKEQZ, &temp2, &temp2, &temp1);
        latxs_append_ir2_opnd3(LISA_MASKNEZ, &temp1, &temp, &temp1);
        latxs_append_ir2_opnd3(LISA_OR, &temp, &temp1, &temp2);
        latxs_ra_free_temp(&temp2);
        /* fcsr all other bits are 0 */
        IR2_OPND fcsr = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_SLLI_D, &fcsr, &temp, 8);

        /* FLAGS/CAUSE 5:2[puoz] -> 7:4[puoz] -> 3:0[zoup] */
        latxs_append_ir2_opnd2i(LISA_SLLI_W, &temp, &new_mxcsr, 2);
        latxs_append_ir2_opnd2(LISA_BITREV_4B, &temp, &temp);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &fcsr, &temp, 27, 24);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &fcsr, &temp, 19, 16);
        /* IE */
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp, &new_mxcsr, 0, 0);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &fcsr, &temp, 28, 28);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &fcsr, &temp, 20, 20);

        if (option_fast_masked_v) {
            latxs_append_ir2_opnd2i(LISA_ORI, &fcsr, &fcsr, 0x10);
        }

        latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr_ir2_opnd, &fcsr);

        latxs_ra_free_temp(&new_mxcsr);
        latxs_ra_free_temp(&fcsr);
        latxs_ra_free_temp(&temp);
        latxs_ra_free_temp(&temp1);

        if (option_lazy_mxcsr) {
            latxs_append_ir2_opnd1(LISA_LABEL, &label2);
        }
    }
#endif
    return true;
}
