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
#include "latx-ghbr.h"

void latxs_sys_logic_register_ir1(void)
{
    latxs_register_ir1(X86_INS_XOR);
    latxs_register_ir1(X86_INS_AND);
    latxs_register_ir1(X86_INS_TEST);
    latxs_register_ir1(X86_INS_OR);
    latxs_register_ir1(X86_INS_NOT);

    latxs_register_ir1(X86_INS_SHL);
    latxs_register_ir1(X86_INS_SHR);
    latxs_register_ir1(X86_INS_SAL);
    latxs_register_ir1(X86_INS_SAR);

    latxs_register_ir1(X86_INS_ROL);
    latxs_register_ir1(X86_INS_ROR);
    latxs_register_ir1(X86_INS_RCL);
    latxs_register_ir1(X86_INS_RCR);

    latxs_register_ir1(X86_INS_SHRD);
    latxs_register_ir1(X86_INS_SHLD);

    latxs_register_ir1(X86_INS_SARX);
    latxs_register_ir1(X86_INS_SHLX);
    latxs_register_ir1(X86_INS_SHRX);

    latxs_register_ir1(X86_INS_BSWAP);
    latxs_register_ir1(X86_INS_RORX);
}

bool latxs_translate_xor(IR1_INST *pir1)
{
    if (latxs_ir1_has_prefix_lock(pir1) &&
        (lsenv->tr_data->sys.cflags & CF_PARALLEL)) {
        return latxs_translate_lock_xor(pir1);
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src0, src1, dest;
    IR2_OPND mem_opnd;
    int imm, opnd0_size;
    bool opt_imm, eflags_calc;

    opnd0_size = ir1_opnd_size(opnd0);
    opt_imm = ir1_opnd_is_s2uimm12(opnd1);
    eflags_calc = ir1_need_calculate_any_flag(pir1);

    /* get src1 */
    if (opt_imm && !eflags_calc) {
        /*
         * If src1 is imm12 and eflags need not calculate,
         * src1 need not load into register.
         */
        src1 = latxs_zero_ir2_opnd;
    } else {
        //src1 = load_ilatxs_translate_andreg_from_ir0(opnd1, UNKNOWN_EXTENSION, false);
        src1 = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    }

    /* get src0 */
    if (ir1_opnd_is_gpr(opnd0)) {
        //src0 = convert_gpr_opnd(opnd0, UNKNOWN_EXTENSION);
        src0 = latxs_load_ireg_from_ir1(opnd0, EXMode_N);
        if (opnd0_size >= 32) {
            dest = src0;
        } else {
            dest = latxs_ra_alloc_itemp();
        }
    } else {
        src0 = latxs_ra_alloc_itemp();
        dest = src0;
        mem_opnd = latxs_ra_alloc_itemp();
        latxs_get_mem_opnd(&mem_opnd, &imm, opnd0, -1);
        la_ld_by_op_size(src0, mem_opnd, imm, opnd0_size, EXMode_N);
    }

    /* set eflag and calculate */
    if (ir1_opnd_is_same_reg(opnd0, opnd1) && ir1_opnd_size(opnd0) >= 32) {
        //la_mov64(dest, zero_ir2_opnd);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &dest, &latxs_zero_ir2_opnd, 63, 0);
        latxs_generate_eflag_calculation(&dest, &dest, &dest, pir1, true);
        return true;
    }

    latxs_generate_eflag_calculation(&dest, &src0, &src1, pir1, true);
    if (opt_imm) {
        //la_xori(dest, src0, ir1_opnd_s2uimm(opnd1));
        latxs_append_ir2_opnd2i(LISA_XORI,&dest, &src0, ir1_opnd_s2uimm(opnd1));
    } else {
        //la_xor(dest, src0, src1);
        latxs_append_ir2_opnd3(LISA_XOR, &dest, &src0, &src1);
    }

    /* write back */
    if (ir1_opnd_is_gpr(opnd0)) {
        /* r32 */
        if (opnd0_size == 32 &&
            !latxs_ghbr_can_elide_write(pir1,
                                        ir1_opnd_base_reg_num(opnd0))) {
            //la_mov32_zx(dest, dest);
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &dest, &dest, 31, 0);
        } else if (opnd0_size < 32) { /* r16/r8 */
            //store_ireg_to_ir1(dest, opnd0, false);
            latxs_store_ir2_to_ir1(&dest, opnd0);
        }
    } else {
        la_st_by_op_size(dest, mem_opnd, imm, opnd0_size);
    }

    return true;
}

/**
 * @brief check imm if it is a maskable value
 * @details Maskable value is a type of value which has consecutive ONEs from
 * the beginning (end) to some position, and other position are ZEROs. For
 * example, 0x00ff (low maskable) and 0xff00 (high maskable) are maskable value,
 * but 0x00ff00 is not. This function can help you check if the value is
 * maskable, also get the type of the maskable value (high maskable/low
 * maskable) and the dividing position between 0 and 1.
 *
 * @param imm input imm
 * @param low if imm is a low maskable value
 * @param pos the mask position
 * @return true if it is a maskable value
 * @return false if not
 */
static inline bool imm_mask(int32_t imm, bool *low, uint32_t *pos)
{
    /* ctz/clz require a nonzero argument. Let ANDI handle the zero mask. */
    if (imm == 0) {
        *low = false;
        *pos = 0;
        return false;
    }

    /* msb is 0 (0x00ff) */
    *low = imm > 0;
    /**
     * First:
     * 0xff00 => 0x00ff => 0x0100
     *        \ neg     \ +1
     * 0x00ff           => 0x0100
     *
     * Then:
     * Check 1 position.
     *
     * |<-clz->|<--ctz-->|
     * 0000 0001 0000 0000  <= 0x0100 (from 0xff00 or 0x00ff)
     * 0000 0111 0000 0000  <= 0x0700 (from 0xf900 or 0x06ff)
     * |<clz>| |<--ctz-->|
     *
     * if (clz + ctz + 1 == len):
     *   maskable vaule
     * else
     *   False
     */

    /* if 0xff00: ~imm + 1; then imm + 1; */
    imm = (*low ? imm : ~imm) + 1;
    /* now src_imm is 0b0..010..0 if in special mode */
    uint32_t t_pos = __builtin_ctz(imm);
    uint32_t l_pos = __builtin_clz(imm);
    if (t_pos + l_pos == sizeof(imm) * 8 - 1) {
       *pos = t_pos;
       return true;
    }
    *pos = 0;
    *low = false;
    return false;
}

bool latxs_translate_and(IR1_INST *pir1)
{
    if (latxs_ir1_has_prefix_lock(pir1) &&
        (lsenv->tr_data->sys.cflags & CF_PARALLEL)) {
        return latxs_translate_lock_and(pir1);
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src0, src1, dest;
    IR2_OPND mem_opnd;
    int imm, opnd0_size;
    bool opt_imm, eflags_calc;

    opnd0_size = ir1_opnd_size(opnd0);
    opt_imm = ir1_opnd_is_s2uimm12(opnd1);
    eflags_calc = ir1_need_calculate_any_flag(pir1);

    /*
     * Find imm is special mode?
     * 0xff00 or 0x00ff
     */
    bool special_mask = false, low_mask = false;
    uint32_t mask_pos = 0;
    /* 8h will load to temp and store, do not need to check */
    if (ir1_opnd_is_imm(opnd1)) {
        special_mask = imm_mask(ir1_opnd_simm(opnd1), &low_mask, &mask_pos);
        opt_imm |= special_mask;

    }

    /* get src1 */
    if (opt_imm && !eflags_calc) {
        /*
         * If src1 is imm12 and eflags need not calculate,
         * src1 need not load into register.
         */
        src1 = latxs_zero_ir2_opnd;

    } else {
        src1 = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    }

    /* get src0 */
    if (ir1_opnd_is_gpr(opnd0)) {
        src0 = latxs_load_ireg_from_ir1(opnd0, EXMode_N);
        /* special_mask can clear origin register directly */
        if (opnd0_size >= 32 || special_mask) {
            dest = src0;
        } else {
            dest = latxs_ra_alloc_itemp();
        }
    } else {
        src0 = latxs_ra_alloc_itemp();
        dest = src0;
        mem_opnd = latxs_ra_alloc_itemp();
        latxs_get_mem_opnd(&mem_opnd, &imm, opnd0, -1);
        la_ld_by_op_size(src0, mem_opnd, imm, opnd0_size, EXMode_N);
    }

    /* set eflag */
    latxs_generate_eflag_calculation(&dest, &src0, &src1, pir1, true);

    /* calculate */
    if (special_mask) {
        if (low_mask) {
            /* low mask, clear high bits */
            int _high_pos = (opnd0_size >= 32) ? 63 : (opnd0_size - 1);
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &dest, &latxs_zero_ir2_opnd, _high_pos, mask_pos);
        } else if (mask_pos > 0) {
            /* high mask, clear low bits (pos cannot be 0 -> 0xff) */
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &dest, &latxs_zero_ir2_opnd, mask_pos - 1, 0);
        }
    } else if (opt_imm) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &dest, &src0, ir1_opnd_s2uimm(opnd1));
    } else {
        latxs_append_ir2_opnd3(LISA_AND, &dest, &src0, &src1);
    }

    /* write back */
    if (ir1_opnd_is_gpr(opnd0)) {

        /* r32, also low_mask will clear high bits */
        if (opnd0_size == 32 && !low_mask &&
            !latxs_ghbr_can_elide_write(pir1,
                                        ir1_opnd_base_reg_num(opnd0))) {
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &dest, &dest, 31, 0);
        } else if (opnd0_size < 32) { /* r16/r8 */
            latxs_store_ir2_to_ir1(&dest, opnd0);
        }
    } else {
        la_st_by_op_size(dest, mem_opnd, imm, opnd0_size);
    }

    return true;
}

bool latxs_translate_test(IR1_INST *pir1)
{
    if (option_by_hand_64 && latxs_translate_test_byhand64(pir1)) {
        return true;
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src0 = latxs_ra_alloc_itemp();
    IR2_OPND src1 = latxs_ra_alloc_itemp();

    latxs_load_ir1_to_ir2(&src0, opnd0, EXMode_S);
    latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_S);

    IR2_OPND dest = latxs_ra_alloc_itemp();

    latxs_append_ir2_opnd3(LISA_AND, &dest, &src0, &src1);

    latxs_generate_eflag_calculation(&dest, &src0, &src1, pir1, true);

    latxs_ra_free_temp(&dest);
    return true;
}

bool latxs_translate_or(IR1_INST *pir1)
{
    if (latxs_ir1_has_prefix_lock(pir1) &&
        (lsenv->tr_data->sys.cflags & CF_PARALLEL)) {
        return latxs_translate_lock_or(pir1);
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src0, src1, dest;
    IR2_OPND mem_opnd;
    int imm, opnd0_size;
    bool opt_imm, eflags_calc;

    opnd0_size = ir1_opnd_size(opnd0);
    opt_imm = ir1_opnd_is_s2uimm12(opnd1);
    eflags_calc = ir1_need_calculate_any_flag(pir1);

    /* get src1 */
    if (opt_imm && !eflags_calc) {
        /*
         * If src1 is imm12 and eflags need not calculate,
         * src1 need not load into register.
         */
        src1 = latxs_zero_ir2_opnd;
    } else {
        //src1 = load_ireg_from_ir1(opnd1, UNKNOWN_EXTENSION, false);
        src1 = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    }

    /* get src0 */
    if (ir1_opnd_is_gpr(opnd0)) {
        //src0 = convert_gpr_opnd(opnd0, UNKNOWN_EXTENSION);
        src0 = latxs_load_ireg_from_ir1(opnd0, EXMode_N);
        if (opnd0_size >= 32) {
            dest = src0;
        } else {
            dest = latxs_ra_alloc_itemp();
        }
    } else {
        src0 = latxs_ra_alloc_itemp();
        dest = src0;
        mem_opnd = latxs_ra_alloc_itemp();
        latxs_get_mem_opnd(&mem_opnd, &imm, opnd0, -1);
        la_ld_by_op_size(src0, mem_opnd, imm, opnd0_size, EXMode_N);
    }

    /* set eflag */
    latxs_generate_eflag_calculation(&dest, &src0, &src1, pir1, true);

    /* calculate */
    if (opt_imm) {
        //la_ori(dest, src0, ir1_opnd_s2uimm(opnd1));
   		latxs_append_ir2_opnd2i(LISA_ORI, &dest, &src0, ir1_opnd_s2uimm(opnd1));
	} else {
        //la_or(dest, src0, src1);
		latxs_append_ir2_opnd3(LISA_OR, &dest, &src0, &src1);
    }

    /* write back */
    if (ir1_opnd_is_gpr(opnd0)) {
        if (opnd0_size == 32 &&
            !latxs_ghbr_can_elide_write(pir1,
                                        ir1_opnd_base_reg_num(opnd0))) {
            //la_mov32_zx(dest, dest);
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &dest, &dest, 31, 0);
        } else if (opnd0_size < 32) { /* r16/r8 */
            //store_ireg_to_ir1(dest, opnd0, false);
            latxs_store_ir2_to_ir1(&dest, opnd0);
        }
    } else {
        la_st_by_op_size(dest, mem_opnd, imm, opnd0_size);
    }

    return true;
}

bool latxs_translate_not(IR1_INST *pir1)
{
    if (latxs_ir1_has_prefix_lock(pir1) &&
        (lsenv->tr_data->sys.cflags & CF_PARALLEL)) {
        return latxs_translate_lock_not(pir1);
    }
    if (option_by_hand_64 && latxs_translate_not_byhand64(pir1)) {
        return true;
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR2_OPND src0 = latxs_ra_alloc_itemp();
    IR2_OPND dest = latxs_ra_alloc_itemp();

    latxs_load_ir1_to_ir2(&src0, opnd0, EXMode_S);

    latxs_append_ir2_opnd2_(lisa_not, &dest, &src0);

    latxs_store_ir2_to_ir1(&dest, opnd0);

    latxs_ra_free_temp(&dest);
    return true;
}

bool latxs_translate_shl(IR1_INST *pir1)
{
    if (option_by_hand_64 && latxs_translate_shl_byhand64(pir1)) {
        return true;
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src, opnd0, EXMode_S);

    IR2_OPND src_shift = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src_shift , opnd1, EXMode_Z);

    int shift_mask = 0x1f;

    if (lsenv->tr_data->sys.code64 && ir1_opnd_size(opnd0) == 64) {
        shift_mask = 0x3f;
    }

    IR2_OPND shift = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ANDI, &shift, &src_shift, shift_mask);
    latxs_ra_free_temp(&src_shift);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    if (!option_imm_cache) {
    latxs_append_ir2_opnd3(LISA_BEQ, &shift,
            &latxs_zero_ir2_opnd, &label_exit);
    }

    IR2_OPND dest = latxs_ra_alloc_itemp();
    if (lsenv->tr_data->sys.code64 && ir1_opnd_size(opnd0) == 64) {
        latxs_append_ir2_opnd3(LISA_SLL_D, &dest, &src, &shift);
    } else {
        latxs_append_ir2_opnd3(LISA_SLL_W, &dest, &src, &shift);
    }

    if (ir1_opnd_is_mem(opnd0)) {
        latxs_store_ir2_to_ir1(&dest, opnd0);
        /* calculate elfags after store */
        latxs_generate_eflag_calculation(&dest, &src, &shift, pir1, true);
        latxs_ra_free_temp(&dest);
        latxs_ra_free_temp(&shift);
    } else {
        /* Destination is GPR, no exception will generate */
        latxs_generate_eflag_calculation(&dest, &src, &shift, pir1, true);
        latxs_ra_free_temp(&src);
        latxs_ra_free_temp(&shift);
        latxs_store_ir2_to_ir1(&dest, opnd0);
        latxs_ra_free_temp(&dest);
    }

    if (!option_imm_cache) {
    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    }

    /* make sure high 32 bits set to zero. TODO: simplify */
    if (ir1_opnd_size(opnd0) == 32 && ir1_opnd_is_gpr(opnd0) &&
        !latxs_ghbr_can_elide_write(pir1, ir1_opnd_base_reg_num(opnd0))) {
        IR2_OPND dest_opnd = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest_opnd, &dest_opnd);
    }

    return true;
}

bool latxs_translate_shr(IR1_INST *pir1)
{
    if (option_by_hand_64 && latxs_translate_shr_byhand64(pir1)) {
        return true;
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    int opsize = ir1_opnd_size(opnd0);
    EXMode em = EXMode_Z;
    if (opsize == 32) {
        em = EXMode_S;
    }

    IR2_OPND src = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src, opnd0, em);

    IR2_OPND src_shift = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src_shift, opnd1, EXMode_Z);

    int shift_mask = 0x1f;

    if (lsenv->tr_data->sys.code64 && ir1_opnd_size(opnd0) == 64) {
        shift_mask = 0x3f;
    }

    IR2_OPND shift = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ANDI, &shift, &src_shift, shift_mask);
    latxs_ra_free_temp(&src_shift);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    if (!option_imm_cache) {
    latxs_append_ir2_opnd3(LISA_BEQ, &shift,
            &latxs_zero_ir2_opnd, &label_exit);
    }

    IR2_OPND dest = latxs_ra_alloc_itemp();

    if (lsenv->tr_data->sys.code64 && ir1_opnd_size(opnd0) == 64) {
        latxs_append_ir2_opnd3(LISA_SRL_D, &dest, &src, &shift);
    } else {
        latxs_append_ir2_opnd3(LISA_SRL_W, &dest, &src, &shift);
    }

    if (ir1_opnd_is_mem(opnd0)) {
        latxs_store_ir2_to_ir1(&dest, opnd0);
        /* calculate elfags after store */
        latxs_generate_eflag_calculation(&dest, &src, &shift, pir1, true);
        latxs_ra_free_temp(&dest);
        latxs_ra_free_temp(&shift);
    } else {
        /* Destination is GPR, no exception will generate */
        latxs_generate_eflag_calculation(&dest, &src, &shift, pir1, true);
        latxs_ra_free_temp(&src);
        latxs_ra_free_temp(&shift);
        latxs_store_ir2_to_ir1(&dest, opnd0);
        latxs_ra_free_temp(&dest);
    }

    if (!option_imm_cache) {
    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    }
    /* make sure high 32 bits set to zero. TODO: simplify */
    if (ir1_opnd_size(opnd0) == 32 && ir1_opnd_is_gpr(opnd0) &&
        !latxs_ghbr_can_elide_write(pir1, ir1_opnd_base_reg_num(opnd0))) {
        IR2_OPND dest_opnd = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest_opnd, &dest_opnd);
    }
    return true;
}

bool latxs_translate_sal(IR1_INST *pir1)
{
    return latxs_translate_shl(pir1);
}

bool latxs_translate_sar(IR1_INST *pir1)
{
    if (option_by_hand_64 && latxs_translate_sar_byhand64(pir1)) {
        return true;
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src = latxs_ra_alloc_itemp();
    IR2_OPND src_shift = latxs_ra_alloc_itemp();

    latxs_load_ir1_to_ir2(&src , opnd0, EXMode_S);
    latxs_load_ir1_to_ir2(&src_shift , opnd1, EXMode_Z);

    int shift_mask = 0x1f;

    if (lsenv->tr_data->sys.code64 && ir1_opnd_size(opnd0) == 64) {
        shift_mask = 0x3f;
    }

    IR2_OPND shift = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ANDI, &shift, &src_shift, shift_mask);
    latxs_ra_free_temp(&src_shift);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    if (!option_imm_cache) {
    latxs_append_ir2_opnd3(LISA_BEQ, &shift,
            &latxs_zero_ir2_opnd, &label_exit);
    }

    IR2_OPND dest = latxs_ra_alloc_itemp();

    if (lsenv->tr_data->sys.code64 && ir1_opnd_size(opnd0) == 64) {
        latxs_append_ir2_opnd3(LISA_SRA_D, &dest, &src, &shift);
    } else {
        latxs_append_ir2_opnd3(LISA_SRA_W, &dest, &src, &shift);
    }

    if (ir1_opnd_is_mem(opnd0)) {
        latxs_store_ir2_to_ir1(&dest, opnd0);
        /* calculate elfags after store */
        latxs_generate_eflag_calculation(&dest, &src, &shift, pir1, true);
        latxs_ra_free_temp(&dest);
        latxs_ra_free_temp(&shift);
    } else {
        /* Destination is GPR, no exception will generate */
        latxs_generate_eflag_calculation(&dest, &src, &shift, pir1, true);
        latxs_ra_free_temp(&src);
        latxs_ra_free_temp(&shift);
        latxs_store_ir2_to_ir1(&dest, opnd0);
        latxs_ra_free_temp(&dest);
    }

    if (!option_imm_cache) {
    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    }
    /* make sure high 32 bits set to zero. TODO: simplify */
    if (ir1_opnd_size(opnd0) == 32 && ir1_opnd_is_gpr(opnd0) &&
        !latxs_ghbr_can_elide_write(pir1, ir1_opnd_base_reg_num(opnd0))) {
        IR2_OPND dest_opnd = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest_opnd, &dest_opnd);
    }
    return true;
}

/* Use native rotation when the guest flags are dead. */
static bool latxs_rotate_imm_no_flags(IR1_INST *pir1, bool left)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    int size = ir1_opnd_size(opnd0);
    if (!lsenv->tr_data->sys.code64 ||
        !ir1_opnd_is_gpr(opnd0) || !ir1_opnd_is_imm(opnd1) ||
        (size != 32 && size != 64) || ir1_need_calculate_any_flag(pir1) ||
        (lsenv->tr_data->sys.cflags & CF_USE_ICOUNT)) return false;

    unsigned count = ir1_opnd_uimm(opnd1) & (size - 1);
    IR2_OPND dest = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
    if (count) {
        latxs_append_ir2_opnd2i(size == 32 ? LISA_ROTRI_W : LISA_ROTRI_D,
                               &dest, &dest, left ? size - count : count);
    }
    if (size == 32) {
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest, &dest);
    }
    return true;
}

bool latxs_translate_rol(IR1_INST *pir1)
{
    if (latxs_rotate_imm_no_flags(pir1, true)) return true;
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src_rotate = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src_rotate, opnd1, EXMode_Z);

    IR2_OPND dest = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&dest, opnd0, EXMode_Z);

    int opnd_size = ir1_opnd_size(opnd0);
    int shift_mask = 0x1f;

    if (lsenv->tr_data->sys.code64 && opnd_size == 64) {
        shift_mask = 0x3f;
    }

    IR2_OPND rotate = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ANDI, &rotate, &src_rotate, shift_mask);
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &rotate, &latxs_zero_ir2_opnd, &label_exit);

    if (ir1_need_calculate_any_flag(pir1)) {
        switch (opnd_size) {
        case 8:
            latxs_append_ir2_opnd2(LISA_X86ROTL_B, &dest, &src_rotate);
            break;
        case 16:
            latxs_append_ir2_opnd2(LISA_X86ROTL_H, &dest, &src_rotate);
            break;
        case 32:
            latxs_append_ir2_opnd2(LISA_X86ROTL_W, &dest, &src_rotate);
            break;
        case 64:
            latxs_append_ir2_opnd2(LISA_X86ROTL_D, &dest, &src_rotate);
            break;
        default:
            lsassert(0);
            break;
        }
    }
    if (latxs_ir2_opnd_is_itemp(&src_rotate)) {
        latxs_ra_free_temp(&src_rotate);
    }

    if (opnd_size == 8) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &rotate, &rotate, 0x7);
    } else if (opnd_size == 16) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &rotate, &rotate, 0xf);
    }

    IR2_OPND label_calc_eflags = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &rotate,
            &latxs_zero_ir2_opnd, &label_calc_eflags);

    IR2_OPND low_dest   = latxs_ra_alloc_itemp();
    IR2_OPND high_dest  = latxs_ra_alloc_itemp();
    IR2_OPND tmp_rotate = latxs_ra_alloc_itemp();

    latxs_append_ir2_opnd3(LISA_SLL_D, &low_dest, &dest, &rotate);
    latxs_append_ir2_opnd3(LISA_SUB_W, &tmp_rotate,
                                        &latxs_zero_ir2_opnd, &rotate);
    latxs_append_ir2_opnd2i(LISA_ANDI, &tmp_rotate,
                                       &tmp_rotate, opnd_size - 1);
    latxs_append_ir2_opnd3(LISA_SRL_D, &high_dest, &dest, &tmp_rotate);
    latxs_append_ir2_opnd3(LISA_OR, &dest, &high_dest, &low_dest);

    latxs_ra_free_temp(&high_dest);
    latxs_ra_free_temp(&low_dest);
    latxs_ra_free_temp(&tmp_rotate);

    latxs_store_ir2_to_ir1(&dest, opnd0);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_calc_eflags);
    latxs_generate_eflag_calculation(&dest, &dest, &rotate, pir1, true);

    /* keep the same with TCG which use CC_OP_ADCOS to compute */
    if (lsenv->tr_data->sys.cflags & CF_USE_ICOUNT) {
        /* CF ← LSB(DEST) */
        /* OF ← MSB(DEST) XOR CF */
        IR2_OPND *t = &rotate;
        latxs_append_ir2_opnd2i(LISA_SRLI_D, t, &dest, opnd_size - 1);
        latxs_append_ir2_opnd3(LISA_XOR, t, t, &dest);
        latxs_append_ir2_opnd2i(LISA_ANDI, t, t, 0x1);
        latxs_append_ir2_opnd2i(LISA_SLLI_D, t, t, 11); /* OF */
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, t, (0x1<<5));
    }

    latxs_ra_free_temp(&dest);
    latxs_ra_free_temp(&rotate);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    /* make sure high 32 bits set to zero. TODO: simplify */
    if (opnd_size == 32 && ir1_opnd_is_gpr(opnd0) &&
        !latxs_ghbr_can_elide_write(pir1, ir1_opnd_base_reg_num(opnd0))) {
        IR2_OPND dest_opnd = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest_opnd, &dest_opnd);
    }
    return true;
}

bool latxs_translate_ror(IR1_INST *pir1)
{
    if (latxs_rotate_imm_no_flags(pir1, false)) return true;
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src_rotate = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src_rotate, opnd1, EXMode_Z);

    IR2_OPND dest = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&dest, opnd0, EXMode_Z);
    int opnd_size = ir1_opnd_size(opnd0);

    int shift_mask = 0x1f;

    if (lsenv->tr_data->sys.code64 && opnd_size == 64) {
        shift_mask = 0x3f;
    }

    IR2_OPND rotate = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ANDI, &rotate, &src_rotate, shift_mask);
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &rotate,
            &latxs_zero_ir2_opnd, &label_exit);

    if (ir1_need_calculate_any_flag(pir1)) {
        switch (opnd_size) {
        case 8:
            latxs_append_ir2_opnd2(LISA_X86ROTR_B, &dest, &src_rotate);
            break;
        case 16:
            latxs_append_ir2_opnd2(LISA_X86ROTR_H, &dest, &src_rotate);
            break;
        case 32:
            latxs_append_ir2_opnd2(LISA_X86ROTR_W, &dest, &src_rotate);
            break;
        case 64:
            latxs_append_ir2_opnd2(LISA_X86ROTR_D, &dest, &src_rotate);
            break;
        default:
            lsassert(0);
            break;
        }
    }
    if (latxs_ir2_opnd_is_itemp(&src_rotate)) {
        latxs_ra_free_temp(&src_rotate);
    }

    if (opnd_size == 8) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &rotate, &rotate, 0x7);
    } else if (opnd_size == 16) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &rotate, &rotate, 0xf);
    }

    IR2_OPND low_dest   = latxs_ra_alloc_itemp();
    IR2_OPND high_dest  = latxs_ra_alloc_itemp();
    IR2_OPND tmp_rotate = latxs_ra_alloc_itemp();

    IR2_OPND label_calc_eflags = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &rotate, &latxs_zero_ir2_opnd,
                                     &label_calc_eflags);
    latxs_append_ir2_opnd3(LISA_SRL_D, &high_dest, &dest, &rotate);
    latxs_append_ir2_opnd3(LISA_SUB_W, &tmp_rotate,
                                       &latxs_zero_ir2_opnd, &rotate);
    latxs_append_ir2_opnd2i(LISA_ANDI, &tmp_rotate,
                                       &tmp_rotate, opnd_size - 1);
    latxs_append_ir2_opnd3(LISA_SLL_D, &low_dest, &dest, &tmp_rotate);
    latxs_append_ir2_opnd3(LISA_OR, &dest, &high_dest, &low_dest);

    latxs_ra_free_temp(&high_dest);
    latxs_ra_free_temp(&low_dest);
    latxs_ra_free_temp(&tmp_rotate);

    latxs_store_ir2_to_ir1(&dest, opnd0);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_calc_eflags);
    latxs_generate_eflag_calculation(&dest, &dest, &rotate, pir1, true);

    /* keep the same with TCG which use CC_OP_ADCOS to compute */
    if (lsenv->tr_data->sys.cflags & CF_USE_ICOUNT) {
        /* CF ← MSB(DEST) */
        /* OF ← MSB(DEST) XOR MSB − 1(DEST) */
        IR2_OPND *t = &rotate;
        IR2_OPND *d = &dest;
        latxs_append_ir2_opnd2i(LISA_SRLI_D, t, d, opnd_size - 1); /* MSB */
        latxs_append_ir2_opnd2i(LISA_SRLI_D, d, d, opnd_size - 2); /* MSB - 1 */
        latxs_append_ir2_opnd3(LISA_XOR, t, t, d);
        latxs_append_ir2_opnd2i(LISA_ANDI, t, t, 0x1);
        latxs_append_ir2_opnd2i(LISA_SLLI_D, t, t, 11); /* OF */
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, t, (0x1<<5));
    }

    latxs_ra_free_temp(&dest);
    latxs_ra_free_temp(&rotate);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    /* make sure high 32 bits set to zero. TODO: simplify */
    if (opnd_size == 32 && ir1_opnd_is_gpr(opnd0) &&
        !latxs_ghbr_can_elide_write(pir1, ir1_opnd_base_reg_num(opnd0))) {
        IR2_OPND dest_opnd = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest_opnd, &dest_opnd);
    }
    return true;
}

bool latxs_translate_rcl(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND dest = latxs_ra_alloc_itemp();
    IR2_OPND src_rotate = latxs_ra_alloc_itemp();

    latxs_load_ir1_to_ir2(&dest, opnd0, EXMode_Z);
    latxs_load_ir1_to_ir2(&src_rotate, opnd1, EXMode_Z);

    int opnd_size = ir1_opnd_size(opnd0);

    IR2_OPND rotate = latxs_ra_alloc_itemp();

    int shift_mask = 0x1f;

    if (lsenv->tr_data->sys.code64 && opnd_size == 64) {
        shift_mask = 0x3f;
    }
    latxs_append_ir2_opnd2i(LISA_ANDI, &rotate, &src_rotate, shift_mask);
    latxs_ra_free_temp(&src_rotate);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &rotate,
            &latxs_zero_ir2_opnd, &label_exit);

    IR2_OPND tmp_imm = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &tmp_imm,
            &latxs_zero_ir2_opnd, opnd_size + 1);
    latxs_append_ir2_opnd3(LISA_MOD_DU, &rotate, &rotate,  &tmp_imm);
    latxs_append_ir2_opnd3(LISA_BEQ, &rotate,
            &latxs_zero_ir2_opnd,  &label_exit);
    latxs_ra_free_temp(&tmp_imm);

    /* this also should work with 32 */
    IR2_OPND tmp_dest   = dest;
    IR2_OPND high_dest  = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_SLL_D, &high_dest, &tmp_dest, &rotate);
    IR2_OPND tmp_rotate = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &tmp_rotate,
                                         &rotate, -1 - opnd_size);
    latxs_append_ir2_opnd3(LISA_SUB_D, &tmp_rotate,
                                       &latxs_zero_ir2_opnd, &tmp_rotate);
    IR2_OPND low_dest   = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_SRL_D, &low_dest, &tmp_dest, &tmp_rotate);
    if (opnd_size == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp_rotate, &tmp_rotate, 0x40);
        latxs_append_ir2_opnd3(LISA_MASKNEZ, &low_dest, &low_dest, &tmp_rotate);
    }

    latxs_ra_free_temp(&tmp_rotate);
    IR2_OPND final_dest = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_OR, &final_dest, &high_dest, &low_dest);
    latxs_ra_free_temp(&low_dest);
    latxs_ra_free_temp(&high_dest);

    IR2_OPND cf = latxs_ra_alloc_itemp();
    latxs_get_eflag_condition(&cf, pir1);
    latxs_append_ir2_opnd3(LISA_SLL_D, &cf, &cf, &rotate);
    latxs_append_ir2_opnd2i(LISA_SRLI_D, &cf, &cf, 1);
    latxs_append_ir2_opnd3(LISA_OR, &final_dest, &final_dest, &cf);
    latxs_ra_free_temp(&cf);

    if (ir1_opnd_is_mem(opnd0)) {
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
        /* calculate elfags after store */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &rotate, pir1, true);
        latxs_ra_free_temp(&rotate);
    } else {
        /* Destination is GPR, no exception will generate */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &rotate, pir1, true);
        latxs_ra_free_temp(&dest);
        latxs_ra_free_temp(&rotate);
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
    }

    latxs_ra_free_temp(&final_dest);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    /* make sure high 32 bits set to zero. TODO: simplify */
    if (opnd_size == 32 && ir1_opnd_is_gpr(opnd0) &&
        !latxs_ghbr_can_elide_write(pir1, ir1_opnd_base_reg_num(opnd0))) {
        IR2_OPND dest_opnd = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest_opnd, &dest_opnd);
    }
    return true;
}

bool latxs_translate_rcr(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND dest = latxs_ra_alloc_itemp();
    IR2_OPND src_rotate = latxs_ra_alloc_itemp();

    latxs_load_ir1_to_ir2(&dest, opnd0, EXMode_Z);
    latxs_load_ir1_to_ir2(&src_rotate, opnd1, EXMode_Z);

    int opnd_size = ir1_opnd_size(opnd0);

    IR2_OPND rotate = latxs_ra_alloc_itemp();

    int shift_mask = 0x1f;

    if (lsenv->tr_data->sys.code64 && opnd_size == 64) {
        shift_mask = 0x3f;
    }

    latxs_append_ir2_opnd2i(LISA_ANDI, &rotate, &src_rotate, shift_mask);
    latxs_ra_free_temp(&src_rotate);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &rotate,
            &latxs_zero_ir2_opnd, &label_exit);

    IR2_OPND tmp_imm = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &tmp_imm,
            &latxs_zero_ir2_opnd, opnd_size + 1);
    latxs_append_ir2_opnd3(LISA_MOD_DU, &rotate, &rotate,  &tmp_imm);
    latxs_append_ir2_opnd3(LISA_BEQ, &rotate,
            &latxs_zero_ir2_opnd, &label_exit);
    latxs_ra_free_temp(&tmp_imm);

    /* this also should work with 32 */
    IR2_OPND tmp_dest = dest;
    IR2_OPND low_dest = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_SRL_D, &low_dest, &tmp_dest, &rotate);
    IR2_OPND tmp_rotate = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &tmp_rotate, &rotate, -1 - opnd_size);
    latxs_append_ir2_opnd3(LISA_SUB_D, &tmp_rotate, &latxs_zero_ir2_opnd,
                           &tmp_rotate);
    IR2_OPND high_dest = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_SLL_D, &high_dest, &tmp_dest, &tmp_rotate);

    if (opnd_size == 64) {
        latxs_append_ir2_opnd2i(LISA_ANDI, &tmp_rotate, &tmp_rotate, 0x40);
        latxs_append_ir2_opnd3(LISA_MASKNEZ, &high_dest, &high_dest,
                               &tmp_rotate);
    }

    IR2_OPND final_dest = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_OR, &final_dest, &high_dest, &low_dest);
    latxs_ra_free_temp(&high_dest);
    latxs_ra_free_temp(&low_dest);
    IR2_OPND cf = latxs_ra_alloc_itemp();
    latxs_get_eflag_condition(&cf, pir1);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &tmp_rotate, &rotate, -opnd_size);
    latxs_append_ir2_opnd3(LISA_SUB_D, &tmp_rotate, &latxs_zero_ir2_opnd,
                           &tmp_rotate);
    latxs_append_ir2_opnd3(LISA_SLL_D, &cf, &cf, &tmp_rotate);
    latxs_append_ir2_opnd3(LISA_OR, &final_dest, &final_dest, &cf);
    latxs_ra_free_temp(&cf);
    latxs_ra_free_temp(&tmp_rotate);

    if (ir1_opnd_is_mem(opnd0)) {
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
        /* calculate elfags after store */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &rotate, pir1, true);
        latxs_ra_free_temp(&rotate);
    } else {
        /* Destination is GPR, no exception will generate */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &rotate, pir1, true);
        latxs_ra_free_temp(&dest);
        latxs_ra_free_temp(&rotate);
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
    }

    latxs_ra_free_temp(&final_dest);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    /* make sure high 32 bits set to zero. TODO: simplify */
    if (opnd_size == 32 && ir1_opnd_is_gpr(opnd0) &&
        !latxs_ghbr_can_elide_write(pir1, ir1_opnd_base_reg_num(opnd0))) {
        IR2_OPND dest_opnd = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest_opnd, &dest_opnd);
    }

    return true;
}

static bool latxs_translate_shrd_cl(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    IR2_OPND dest = latxs_ra_alloc_itemp();
    IR2_OPND src = latxs_ra_alloc_itemp();
    IR2_OPND shift = latxs_ra_alloc_itemp();

    latxs_load_ir1_to_ir2(&dest, opnd0, EXMode_Z);
    latxs_load_ir1_to_ir2(&src, opnd1, EXMode_Z);
    latxs_load_ir1_to_ir2(&shift, opnd2, EXMode_Z);

    int opnd_size = ir1_opnd_size(opnd0);

    int shift_mask = 0x1f;
    if (lsenv->tr_data->sys.code64 && opnd_size == 64) {
        shift_mask = 0x3f;
    }
    latxs_append_ir2_opnd2i(LISA_ANDI, &shift, &shift, shift_mask);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &shift,
            &latxs_zero_ir2_opnd, &label_exit);

    IR2_OPND left_shift = latxs_ra_alloc_itemp();
    IR2_OPND size = latxs_ra_alloc_itemp();
    latxs_load_imm32_to_ir2(&size, shift_mask + 1, EXMode_S);
    latxs_append_ir2_opnd3(LISA_SUB_W, &left_shift, &size, &shift);
    latxs_ra_free_temp(&size);

    if (opnd_size == 16) {
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &dest, &src, 31, 16);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &src, &dest, 15, 0);
    }
    IR2_OPND high_dest  = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_SLL_D, &high_dest, &src, &left_shift);
    latxs_ra_free_temp(&left_shift);
    latxs_ra_free_temp(&src);

    IR2_OPND low_dest   = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_SRL_D, &low_dest, &dest, &shift);

    IR2_OPND final_dest = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_OR, &final_dest, &high_dest, &low_dest);

    latxs_ra_free_temp(&low_dest);
    latxs_ra_free_temp(&high_dest);

    if (ir1_opnd_is_mem(opnd0)) {
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
        /* calculate elfags after store */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &shift, pir1, true);
        latxs_ra_free_temp(&shift);
    } else {
        /* Destination is GPR, no exception will generate */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &shift, pir1, true);
        latxs_ra_free_temp(&dest);
        latxs_ra_free_temp(&shift);
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
    }

    latxs_ra_free_temp(&final_dest);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    /* make sure high 32 bits set to zero. TODO: simplify */
    if (opnd_size == 32 && ir1_opnd_is_gpr(opnd0)) {
        IR2_OPND dest_opnd = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest_opnd, &dest_opnd);
    }

    return true;
}

static bool latxs_translate_shrd_imm(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

#ifdef CONFIG_SOFTMMU
    lsassertm_illop(ir1_addr(pir1), ir1_opnd_is_imm(opnd2),
            "shrd imm operand2 is not imm.\n");
#endif

    int opnd_size = ir1_opnd_size(opnd0);
    int shift_mask = 0x1f;
    if (lsenv->tr_data->sys.code64 && opnd_size == 64) {
        shift_mask = 0x3f;
    }

    int shift = ir1_opnd_simm(opnd2) & shift_mask;
    if (!shift) {
        if (opnd_size == 32 && ir1_opnd_is_gpr(opnd0)) {
            IR2_OPND dest_opnd =
                latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
            latxs_append_ir2_opnd2_(lisa_mov32z, &dest_opnd, &dest_opnd);
        }
        return true;
    }

    IR2_OPND dest = latxs_ra_alloc_itemp();
    IR2_OPND src  = latxs_ra_alloc_itemp();

    latxs_load_ir1_to_ir2(&dest, opnd0, EXMode_Z);
    latxs_load_ir1_to_ir2(&src , opnd1, EXMode_Z);

    if (opnd_size == 16) {
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &dest, &src, 31, 16);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &src, &dest, 15, 0);
    }
    IR2_OPND low_dest = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_SRLI_D, &low_dest, &dest, shift);

    int left_shift = shift_mask + 1 - shift;
    //if (opnd_size == 16 && shift > 16) {
    //    fprintf(stderr, "[warning] <%s> %s:%d : ", __func__,
    //                                         __FILE__, __LINE__);
    //    fprintf(stderr, "undefined behaviour, shrd\n");
    //    ir1_dump(pir1);
    //}
    IR2_OPND high_dest = latxs_ra_alloc_itemp();
    IR2_OPND final_dest = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_SLLI_D, &high_dest, &src, left_shift);
    latxs_append_ir2_opnd3(LISA_OR, &final_dest, &high_dest, &low_dest);
    latxs_ra_free_temp(&src);
    latxs_ra_free_temp(&low_dest);
    latxs_ra_free_temp(&high_dest);

    IR2_OPND shift_opnd = latxs_ir2_opnd_new(IR2_OPND_IMMH, (int16)shift);
    if (ir1_opnd_is_mem(opnd0)) {
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
        /* calculate elfags after store */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &shift_opnd, pir1, true);
    } else {
        /* Destination is GPR, no exception will generate */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &shift_opnd, pir1, true);
        latxs_ra_free_temp(&dest);
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
    }

    latxs_ra_free_temp(&final_dest);

    return true;
}

bool latxs_translate_shrd(IR1_INST *pir1)
{
    if (ir1_opnd_is_imm(ir1_get_opnd(pir1, 0) + 2)) {
        return latxs_translate_shrd_imm(pir1);
    } else {
        return latxs_translate_shrd_cl(pir1);
    }

    return true;
}

static bool latxs_translate_shld_cl(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    IR2_OPND dest = latxs_ra_alloc_itemp();
    IR2_OPND src = latxs_ra_alloc_itemp();
    IR2_OPND shift = latxs_ra_alloc_itemp();

    latxs_load_ir1_to_ir2(&dest, opnd0, EXMode_Z);
    latxs_load_ir1_to_ir2(&src, opnd1, EXMode_Z);
    latxs_load_ir1_to_ir2(&shift, opnd2, EXMode_Z);

    int opnd_size = ir1_opnd_size(opnd0);

    int shift_mask = 0x1f;
    if (lsenv->tr_data->sys.code64 && opnd_size == 64) {
        shift_mask = 0x3f;
    }
    latxs_append_ir2_opnd2i(LISA_ANDI, &shift, &shift, shift_mask);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &shift,
            &latxs_zero_ir2_opnd, &label_exit);

    IR2_OPND left_shift = latxs_ra_alloc_itemp();
    IR2_OPND size       = latxs_ra_alloc_itemp();
    latxs_load_imm32_to_ir2(&size, shift_mask + 1, EXMode_S);
    latxs_append_ir2_opnd3(LISA_SUB_W, &left_shift, &size, &shift);
    latxs_ra_free_temp(&size);

    if (opnd_size == 16) {
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &src, &src, 31, 16);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &src, &dest, 15, 0);
    }

    IR2_OPND low_dest   = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_SRL_D, &low_dest, &src, &left_shift);
    latxs_ra_free_temp(&left_shift);

    IR2_OPND high_dest  = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_SLL_D, &high_dest, &dest, &shift);
    IR2_OPND final_dest = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_OR, &final_dest, &high_dest, &low_dest);

    latxs_ra_free_temp(&low_dest);
    latxs_ra_free_temp(&high_dest);

    if (opnd_size == 16) {
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &dest, &dest, 31, 16);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &src, &src, 31, 16);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &dest, &src, 15, 0);
    }
    latxs_ra_free_temp(&src);

    if (ir1_opnd_is_mem(opnd0)) {
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
        /* calculate elfags after store */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &shift, pir1, true);
        latxs_ra_free_temp(&shift);
    } else {
        /* Destination is GPR, no exception will generate */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &shift, pir1, true);
        latxs_ra_free_temp(&shift);
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
    }

    latxs_ra_free_temp(&final_dest);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    /* make sure high 32 bits set to zero. TODO: simplify */
    if (opnd_size == 32 && ir1_opnd_is_gpr(opnd0)) {
        IR2_OPND dest_opnd = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest_opnd, &dest_opnd);
    }
    return true;
}

static bool latxs_translate_shld_imm(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

#ifdef CONFIG_SOFTMMU
    lsassertm_illop(ir1_addr(pir1), ir1_opnd_is_imm(opnd2),
            "shld imm operand2 is not imm.\n");
#endif

    int opnd_size = ir1_opnd_size(opnd0);
    int shift_mask = 0x1f;
    if (lsenv->tr_data->sys.code64 && opnd_size == 64) {
        shift_mask = 0x3f;
    }
    int shift = ir1_opnd_simm(opnd2) & shift_mask;
    if (!shift) {
        if (opnd_size == 32 && ir1_opnd_is_gpr(opnd0)) {
            IR2_OPND dest_opnd =
                latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
            latxs_append_ir2_opnd2_(lisa_mov32z, &dest_opnd, &dest_opnd);
        }
        return true;
    }

    IR2_OPND dest = latxs_ra_alloc_itemp();
    IR2_OPND src  = latxs_ra_alloc_itemp();

    latxs_load_ir1_to_ir2(&dest, opnd0, EXMode_Z);
    latxs_load_ir1_to_ir2(&src , opnd1, EXMode_Z);

    if (opnd_size == 16) {
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &src, &src, 31, 16);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &src, &dest, 15, 0);
    }
    IR2_OPND high_dest = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_SLLI_D, &high_dest, &dest, shift);

    int left_shift = shift_mask + 1 - shift;

    IR2_OPND low_dest = latxs_ra_alloc_itemp();
    IR2_OPND final_dest = latxs_ra_alloc_itemp();

    latxs_append_ir2_opnd2i(LISA_SRLI_D, &low_dest, &src, left_shift);
    latxs_append_ir2_opnd3(LISA_OR, &final_dest, &high_dest, &low_dest);

    latxs_ra_free_temp(&low_dest);
    latxs_ra_free_temp(&high_dest);

    if (opnd_size == 16) {
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &dest, &dest, 31, 16);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &src, &src, 31, 16);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &dest, &src, 15, 0);
    }
    latxs_ra_free_temp(&src);

    IR2_OPND shift_opnd = latxs_ir2_opnd_new(IR2_OPND_IMMH, (int16)shift);
    if (ir1_opnd_is_mem(opnd0)) {
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
        /* calculate elfags after store */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &shift_opnd, pir1, true);
    } else {
        /* Destination is GPR, no exception will generate */
        latxs_generate_eflag_calculation(&final_dest,
                &dest, &shift_opnd, pir1, true);
        latxs_ra_free_temp(&dest);
        latxs_store_ir2_to_ir1(&final_dest, opnd0);
    }

    latxs_ra_free_temp(&final_dest);

    return true;
}

bool latxs_translate_shld(IR1_INST *pir1)
{
    if (ir1_opnd_is_imm(ir1_get_opnd(pir1, 0) + 2)) {
        return latxs_translate_shld_imm(pir1);
    } else {
        return latxs_translate_shld_cl(pir1);
    }

    return true;
}

bool latxs_translate_bswap(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);

    int opnd_size = ir1_opnd_size(opnd0);

    IR2_OPND value = latxs_ra_alloc_itemp();

    if (opnd_size == 16) {
        /* qemu just perform 32bit swap */
        /* real machine perform set low 16 bit to zero */
        /* eg. 66 0f c8 */
        //fprintf(stderr, "[warning] <%s> %s:%d : ", __func__,
        //                            __FILE__, __LINE__);
        //fprintf(stderr, "undefined behaviour, bswap\n");
        //ir1_dump(pir1);
        ///* TODO : common log(info/warning) */
        //opnd0->size = 4;

        opnd0->size = (32 >> 3);
        switch (opnd0->reg) {
        case X86_REG_AX:
            opnd0->reg = X86_REG_EAX;
            break;
        case X86_REG_BX:
            opnd0->reg = X86_REG_EBX;
            break;
        case X86_REG_CX:
            opnd0->reg = X86_REG_ECX;
            break;
        case X86_REG_DX:
            opnd0->reg = X86_REG_EDX;
            break;
        case X86_REG_SP:
            opnd0->reg = X86_REG_ESP;
            break;
        case X86_REG_BP:
            opnd0->reg = X86_REG_EBP;
            break;
        case X86_REG_SI:
            opnd0->reg = X86_REG_ESI;
            break;
        case X86_REG_DI:
            opnd0->reg = X86_REG_EDI;
            break;
        case X86_REG_R8W:
            opnd0->reg = X86_REG_R8D;
            break;
        case X86_REG_R9W:
            opnd0->reg = X86_REG_R9D;
            break;
        case X86_REG_R10W:
            opnd0->reg = X86_REG_R10D;
            break;
        case X86_REG_R11W:
            opnd0->reg = X86_REG_R11D;
            break;
        case X86_REG_R12W:
            opnd0->reg = X86_REG_R12D;
            break;
        case X86_REG_R13W:
            opnd0->reg = X86_REG_R13D;
            break;
        case X86_REG_R14W:
            opnd0->reg = X86_REG_R14D;
            break;
        case X86_REG_R15W:
            opnd0->reg = X86_REG_R15D;
            break;
        default:
            lsassert(0);
            break;
        }
    }

    latxs_load_ir1_to_ir2(&value, opnd0, EXMode_Z);

    if (opnd_size == 64) {
        latxs_append_ir2_opnd2(LISA_REVB_D, &value, &value);
    } else if (opnd_size == 16) {
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &value, &latxs_zero_ir2_opnd, 15, 0);
    } else {
        latxs_append_ir2_opnd2(LISA_REVB_2W, &value, &value);
    }

    latxs_store_ir2_to_ir1(&value, opnd0);

    latxs_ra_free_temp(&value);
    return true;
}

bool latxs_translate_sarx(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0); /* gpr */
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1); /* gpr/mem */
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2); /* gpr */

    IR2_OPND dest, src, offset;

    dest = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
    offset = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd2));

    int os = ir1_opnd_size(opnd0);
    lsassert(os == 64 || os == 32);

    if (ir1_opnd_is_gpr(opnd1)) {
        src = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd1));
    } else {
        lsassert(ir1_opnd_is_mem(opnd1));
        src = latxs_ra_alloc_itemp();
        latxs_load_ir1_mem_to_ir2(&src, opnd1, EXMode_N, -1);
    }

    if (os == 64) {
        latxs_append_ir2_opnd3(LISA_SRA_D, &dest, &src, &offset);
    } else {
        latxs_append_ir2_opnd3(LISA_SRA_W, &dest, &src, &offset);
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest, &dest);
    }

    return true;
}

bool latxs_translate_shlx(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0); /* gpr */
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1); /* gpr/mem */
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2); /* gpr */

    IR2_OPND dest, src, offset;

    dest = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
    offset = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd2));

    int os = ir1_opnd_size(opnd0);
    lsassert(os == 64 || os == 32);

    if (ir1_opnd_is_gpr(opnd1)) {
        src = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd1));
    } else {
        lsassert(ir1_opnd_is_mem(opnd1));
        src = latxs_ra_alloc_itemp();
        latxs_load_ir1_mem_to_ir2(&src, opnd1, EXMode_N, -1);
    }

    if (os == 64) {
        latxs_append_ir2_opnd3(LISA_SLL_D, &dest, &src, &offset);
    } else {
        latxs_append_ir2_opnd3(LISA_SLL_W, &dest, &src, &offset);
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest, &dest);
    }

    return true;
}

bool latxs_translate_shrx(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0); /* gpr */
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1); /* gpr/mem */
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2); /* gpr */

    IR2_OPND dest, src, offset;

    dest = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
    offset = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd2));

    int os = ir1_opnd_size(opnd0);
    lsassert(os == 64 || os == 32);

    if (ir1_opnd_is_gpr(opnd1)) {
        src = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd1));
    } else {
        lsassert(ir1_opnd_is_mem(opnd1));
        src = latxs_ra_alloc_itemp();
        latxs_load_ir1_mem_to_ir2(&src, opnd1, EXMode_N, -1);
    }

    if (os == 64) {
        latxs_append_ir2_opnd3(LISA_SRL_D, &dest, &src, &offset);
    } else {
        latxs_append_ir2_opnd3(LISA_SRL_W, &dest, &src, &offset);
        latxs_append_ir2_opnd2_(lisa_mov32z, &dest, &dest);
    }

    return true;
}

bool latxs_translate_rorx(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR2_OPND dest = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
    //IR2_OPND src = load_ireg_from_ir1(ir1_get_opnd(pir1, 1), ZERO_EXTENSION, false);
    IR2_OPND src = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src, ir1_get_opnd(pir1, 1), EXMode_Z);
    //IR2_OPND imm = load_ireg_from_ir1(ir1_get_opnd(pir1, 2), ZERO_EXTENSION, false);
    IR2_OPND imm = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&imm, ir1_get_opnd(pir1, 2), EXMode_Z);
    int dest_size = ir1_opnd_size(opnd0);
    IR2_OPND count = latxs_ra_alloc_itemp();
    IR2_OPND tmp = latxs_ra_alloc_itemp();
    if (dest_size == 32) {
        //la_andi(count, imm, 0x1f);
        latxs_append_ir2_opnd2i(LISA_ANDI, &count, &imm, 0x1f);
        //la_rotr_w(tmp, src, count);
        latxs_append_ir2_opnd3(LISA_ROTR_W, &tmp, &src, &count);
        //la_bstrpick_d(dest, tmp, 31, 0);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &dest, &tmp, 31, 0);
    } else if (dest_size == 64) {
        //la_andi(count, imm, 0x3f);
        latxs_append_ir2_opnd2i(LISA_ANDI, &count, &imm, 0x3f);
        //la_rotr_d(dest, src, count);
        latxs_append_ir2_opnd3(LISA_ROTR_D, &dest, &src, &count);
    } else {
        lsassert(0);
    }

    return true;
}
