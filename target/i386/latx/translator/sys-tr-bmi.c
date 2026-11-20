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
#include "sys-excp.h"

void latxs_sys_bmi_register_ir1(void)
{
    latxs_register_ir1(X86_INS_BEXTR);
    latxs_register_ir1(X86_INS_BZHI);
    latxs_register_ir1(X86_INS_BLSI);
    latxs_register_ir1(X86_INS_BLSMSK);
    latxs_register_ir1(X86_INS_BLSR);
    latxs_register_ir1(X86_INS_PDEP);
    latxs_register_ir1(X86_INS_PEXT);
    latxs_register_ir1(X86_INS_SALC);
}

bool latxs_translate_bextr(IR1_INST *pir1)
{
    //IR2_OPND src_opnd = load_ireg_from_ir1(ir1_get_opnd(pir1, 1), ZERO_EXTENSION, false);
    IR2_OPND src_opnd = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src_opnd, ir1_get_opnd(pir1, 1), EXMode_Z);
    //IR2_OPND ctrl_opnd = load_ireg_from_ir1(ir1_get_opnd(pir1, 2), ZERO_EXTENSION, false);
    IR2_OPND ctrl_opnd = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&ctrl_opnd, ir1_get_opnd(pir1, 2), EXMode_Z);
    
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    IR2_OPND label_zf1 = latxs_ir2_opnd_new_label();
    int bound = (ir1_opnd_size(ir1_get_opnd(pir1, 0)) == 32) ? 32: 64;

    IR2_OPND start_opnd = latxs_ra_alloc_itemp();
    //la_bstrpick_d(start_opnd, ctrl_opnd, 7, 0);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &start_opnd, &ctrl_opnd, 7, 0);
    //la_bstrpick_d(len_opnd, ctrl_opnd, 15, 8);
    IR2_OPND len_opnd = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &len_opnd, &ctrl_opnd, 15, 8);
    latxs_ra_free_temp(&ctrl_opnd);

    //la_srl_d(temp1_opnd, src_opnd, start_opnd);
    IR2_OPND temp1_opnd = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_SRL_D, &temp1_opnd, &src_opnd, &start_opnd);
    latxs_ra_free_temp(&src_opnd);
    //la_sltui(slt_opnd, start_opnd, bound);
    IR2_OPND slt_opnd = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_SLTUI, &slt_opnd, &start_opnd, bound);
    //ra_free_temp(start_opnd);
    latxs_ra_free_temp(&start_opnd);
    //la_maskeqz(temp1_opnd, temp1_opnd, slt_opnd);
    latxs_append_ir2_opnd3(LISA_MASKEQZ, &temp1_opnd, &temp1_opnd, &slt_opnd);

    //li_d(temp2_opnd, -1);
    IR2_OPND temp2_opnd = latxs_ra_alloc_itemp();
    latxs_load_imm64_to_ir2(&temp2_opnd, -1);

    //la_sll_d(temp2_opnd, temp2_opnd, len_opnd);
    latxs_append_ir2_opnd3(LISA_SLL_D, &temp2_opnd, &temp2_opnd, &len_opnd);
    //la_sltui(slt_opnd, len_opnd, bound);
    latxs_append_ir2_opnd2i(LISA_SLTUI, &slt_opnd, &len_opnd, bound);
    //ra_free_temp(len_opnd);
    latxs_ra_free_temp(&len_opnd);
    //la_maskeqz(temp2_opnd, temp2_opnd, slt_opnd);
    latxs_append_ir2_opnd3(LISA_MASKEQZ, &temp2_opnd, &temp2_opnd, &slt_opnd);
    latxs_ra_free_temp(&slt_opnd);

    //la_andn(temp2_opnd, temp1_opnd, temp2_opnd);
    latxs_append_ir2_opnd3(LISA_ANDN, &temp2_opnd, &temp1_opnd, &temp2_opnd);
    //ra_free_temp(temp1_opnd);
    latxs_ra_free_temp(&temp1_opnd);

    /* 2. set eflags */
    //la_beq(temp2_opnd, zero_ir2_opnd, label_zf1);
    latxs_append_ir2_opnd3(LISA_BEQ, &temp2_opnd, &latxs_zero_ir2_opnd, &label_zf1);
    /* set zf = 0 */
    //la_x86mtflag(zero_ir2_opnd, 0x8);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_zero_ir2_opnd, 0x8);
    //la_b(label_exit);
    latxs_append_ir2_opnd1(LISA_B, &label_exit);
    /* set zf = 1 */
    //la_label(label_zf1);
    latxs_append_ir2_opnd1(LISA_LABEL, &label_zf1);
    //IR2_OPND n4095_opnd = ra_alloc_num_4095();
    IR2_OPND n4095_opnd = latxs_ra_alloc_itemp();             
    latxs_append_ir2_opnd2i(LISA_ORI, &n4095_opnd, &latxs_zero_ir2_opnd, 0xfff);
    //la_x86mtflag(n4095_opnd, 0x8);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &n4095_opnd, 0x8);
    //ra_free_num_4095(n4095_opnd);
    latxs_ra_free_temp(&n4095_opnd);
    //la_label(label_exit);
    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    /* set cf = 0, of = 0 */
    //la_x86mtflag(zero_ir2_opnd, 0x21);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_zero_ir2_opnd, 0x21);
    /* 3. store result */
    //store_ireg_to_ir1(temp2_opnd, ir1_get_opnd(pir1, 0), false);
    latxs_store_ir2_to_ir1(&temp2_opnd, ir1_get_opnd(pir1, 0));
    return true;
}

bool latxs_translate_bzhi(IR1_INST *pir1)
{
    //IR2_OPND src_opnd = load_ireg_from_ir1(ir1_get_opnd(pir1, 1), ZERO_EXTENSION, false);
    IR2_OPND src_opnd = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src_opnd, ir1_get_opnd(pir1, 1), EXMode_Z);
    //IR2_OPND ctrl_opnd = load_ireg_from_ir1(ir1_get_opnd(pir1, 2), ZERO_EXTENSION, false);
    latxs_append_ir2_opnd2i(LISA_ORI, &latxs_zero_ir2_opnd, &latxs_zero_ir2_opnd, 0x0);
    IR2_OPND ctrl_opnd = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&ctrl_opnd, ir1_get_opnd(pir1, 2), EXMode_Z);
    IR2_OPND n_opnd = latxs_ra_alloc_itemp();
    IR2_OPND slt_opnd = latxs_ra_alloc_itemp();
    IR2_OPND temp1_opnd = latxs_ra_alloc_itemp();
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();

    int opnd0_size = ir1_opnd_size(ir1_get_opnd(pir1, 0));
    int bound = (opnd0_size == 32) ? 32: 64;

    //la_bstrpick_d(n_opnd, ctrl_opnd, 7, 0);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &n_opnd, &ctrl_opnd, 7, 0);

    //li_d(temp1_opnd, -1);
    latxs_load_imm64_to_ir2(&temp1_opnd, -1);
    //la_sll_d(temp1_opnd, temp1_opnd, n_opnd);
    latxs_append_ir2_opnd3(LISA_SLL_D, &temp1_opnd, &temp1_opnd, &n_opnd);
    //la_sltui(slt_opnd, n_opnd, bound); 
    latxs_append_ir2_opnd2i(LISA_SLTUI, &slt_opnd, &n_opnd, bound);
    //la_maskeqz(temp1_opnd, temp1_opnd, slt_opnd);
    latxs_append_ir2_opnd3(LISA_MASKEQZ, &temp1_opnd, &temp1_opnd, &slt_opnd);
    //la_andn(temp1_opnd, src_opnd, temp1_opnd);
    latxs_append_ir2_opnd3(LISA_ANDN, &temp1_opnd, &src_opnd, &temp1_opnd);

    /* 2. set eflags */
    //generate_eflag_calculation(temp1_opnd, temp1_opnd, temp1_opnd, pir1, true);
    latxs_generate_eflag_calculation(&temp1_opnd, &temp1_opnd, &temp1_opnd, pir1, true);

    /* IF (N > OperandSize - 1) CF ← 1 */
    IR2_OPND temp2_opnd = latxs_ra_alloc_itemp();
    //li_d(temp2_opnd, opnd0_size);
    latxs_load_imm64_to_ir2(&temp2_opnd, opnd0_size);
    //la_addi_d(temp2_opnd, temp2_opnd, -1);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &temp2_opnd, &temp2_opnd, -1);
    //la_bge(temp2_opnd, n_opnd, label_exit);
    latxs_append_ir2_opnd3(LISA_BGE, &temp2_opnd, &n_opnd, &label_exit);

    latxs_ra_free_temp(&src_opnd);
    latxs_ra_free_temp(&ctrl_opnd);
    latxs_ra_free_temp(&n_opnd);
    latxs_ra_free_temp(&slt_opnd);
    /* set cf = 1 */
    //IR2_OPND n4095_opnd = ra_alloc_num_4095();
    IR2_OPND n4095_opnd = latxs_ra_alloc_itemp();             
    latxs_append_ir2_opnd2i(LISA_ORI, &n4095_opnd, &latxs_zero_ir2_opnd, 0xfff);
    //la_x86mtflag(n4095_opnd, CF_USEDEF_BIT);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &n4095_opnd, CF_USEDEF_BIT);
    //ra_free_num_4095(n4095_opnd);
    latxs_ra_free_temp(&n4095_opnd);

    /* 3. store result */
    //la_label(label_exit);
    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    //store_ireg_to_ir1(temp1_opnd, ir1_get_opnd(pir1, 0), false);
    latxs_store_ir2_to_ir1(&temp1_opnd, ir1_get_opnd(pir1, 0));
    return true;
}

bool latxs_translate_blsi(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dest = latxs_load_ireg_from_ir1(opnd0, EXMode_N);
    IR2_OPND src = latxs_load_ireg_from_ir1(opnd1, EXMode_N);

    IR2_OPND temp = latxs_ra_alloc_itemp();
    IR2_OPND src_temp = latxs_ra_alloc_itemp();
    if (ir1_opnd_is_same_reg(opnd0, opnd1)) {
        //la_or(src_temp, zero_ir2_opnd, src);
        latxs_append_ir2_opnd3(LISA_OR, &src_temp, &latxs_zero_ir2_opnd, &src);
    }
    if (ir1_opnd_size(opnd0) == 32) {
        //la_sub_w(temp, zero_ir2_opnd, src);
        latxs_append_ir2_opnd3(LISA_SUB_W, &temp, &latxs_zero_ir2_opnd, &src);
        //la_and(dest, temp, src);
        latxs_append_ir2_opnd3(LISA_AND, &dest, &temp, &src);
        //la_bstrpick_d(dest, dest, 31, 0);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &dest, &dest, 31, 0);
    } else {
        //la_sub_d(temp, zero_ir2_opnd, src);
        latxs_append_ir2_opnd3(LISA_SUB_D, &temp, &latxs_zero_ir2_opnd, &src);
        //la_and(dest, temp, src);
        latxs_append_ir2_opnd3(LISA_AND, &dest, &temp, &src);
    }

    if (ir1_opnd_is_same_reg(opnd0, opnd1)) {
        //generate_eflag_calculation(dest, dest, src_temp, pir1, true);
        latxs_generate_eflag_calculation(&dest, &dest, &src_temp, pir1, true);
    } else {
        //generate_eflag_calculation(dest, dest, src, pir1, true);
        latxs_generate_eflag_calculation(&dest, &dest, &src, pir1, true);
    }
    latxs_ra_free_temp(&temp);
    latxs_ra_free_temp(&src_temp);

    return true;
}

bool latxs_translate_blsmsk(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dest = latxs_load_ireg_from_ir1(opnd0, EXMode_N);
    IR2_OPND src = latxs_load_ireg_from_ir1(opnd1, EXMode_N);

    IR2_OPND temp = latxs_ra_alloc_itemp();
    IR2_OPND src_temp = latxs_ra_alloc_itemp();
    if (ir1_opnd_is_same_reg(opnd0, opnd1)) {
        //la_or(src_temp, zero_ir2_opnd, src);
        latxs_append_ir2_opnd3(LISA_OR, &src_temp, &latxs_zero_ir2_opnd, &src);
    }
    if (ir1_opnd_size(opnd0) == 32) {
        //la_addi_w(temp, src, -1);
        latxs_append_ir2_opnd2i(LISA_ADDI_W, &temp, &src, -1);
        //la_xor(dest, temp, src);
        latxs_append_ir2_opnd3(LISA_XOR, &dest, &temp, &src);
        //la_bstrpick_d(dest, dest, 31, 0);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &dest, &dest, 31, 0);
    } else {
        //la_addi_d(temp, src, -1);
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &temp, &src, -1);
        //la_xor(dest, temp, src);
        latxs_append_ir2_opnd3(LISA_XOR, &dest, &temp, &src);
    }

    if (ir1_opnd_is_same_reg(opnd0, opnd1)) {
        //generate_eflag_calculation(dest, temp, src_temp, pir1, true);
        latxs_generate_eflag_calculation(&dest, &temp, &src_temp, pir1, true);
    } else {
        //generate_eflag_calculation(dest, temp, src, pir1, true);
        latxs_generate_eflag_calculation(&dest, &temp, &src, pir1, true);
    }
    latxs_ra_free_temp(&temp);
    latxs_ra_free_temp(&src_temp);

    return true;
}

bool latxs_translate_blsr(IR1_INST *pir1)
{
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dest = latxs_load_ireg_from_ir1(opnd0, EXMode_N);
    IR2_OPND src = latxs_load_ireg_from_ir1(opnd1, EXMode_N);

    IR2_OPND temp = latxs_ra_alloc_itemp();
    IR2_OPND src_temp = latxs_ra_alloc_itemp();
    if (ir1_opnd_is_same_reg(opnd0, opnd1)) {
        //la_or(src_temp, zero_ir2_opnd, src);
        latxs_append_ir2_opnd3(LISA_OR, &src_temp, &latxs_zero_ir2_opnd, &src);
    }
    if (ir1_opnd_size(opnd0) == 32) {
        //la_addi_w(temp, src, -1);
        latxs_append_ir2_opnd2i(LISA_ADDI_W, &temp, &src, -1);
        //la_and(dest, temp, src);
        latxs_append_ir2_opnd3(LISA_AND, &dest, &temp, &src);
        //la_bstrpick_d(dest, dest, 31, 0);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &dest, &dest, 31, 0);
    } else {
        //la_addi_d(temp, src, -1);
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &temp, &src, -1);
        //la_and(dest, temp, src);
        latxs_append_ir2_opnd3(LISA_AND, &dest, &temp, &src);
    }
    latxs_ra_free_temp(&temp);

    if (ir1_opnd_is_same_reg(opnd0, opnd1)) {
        //generate_eflag_calculation(dest, dest, src_temp, pir1, true);
        latxs_generate_eflag_calculation(&dest, &dest, &src_temp, pir1, true);
    } else {
        //generate_eflag_calculation(dest, dest, src, pir1, true);
        latxs_generate_eflag_calculation(&dest, &dest, &src, pir1, true);
    }

    latxs_ra_free_temp(&src_temp);

    return true;
}

bool latxs_translate_pdep(IR1_INST *pir1)
{
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    IR2_OPND label_ctz = latxs_ir2_opnd_new_label();
    // IR2_OPND src_opnd =
    //     load_ireg_from_ir1(ir1_get_opnd(pir1, 1), ZERO_EXTENSION, false);
    IR2_OPND src_opnd = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(ir1_get_opnd(pir1, 1)));

    IR2_OPND mask_opnd = latxs_ra_alloc_itemp();
    //load_ireg_from_ir1_2(mask_opnd, ir1_get_opnd(pir1, 2), ZERO_EXTENSION, false);
    latxs_load_ir1_to_ir2(&mask_opnd, ir1_get_opnd(pir1, 2), EXMode_Z);
    IR2_OPND temp1_opnd = latxs_ra_alloc_itemp();
    IR2_OPND temp2_opnd = latxs_ra_alloc_itemp();
    IR2_OPND temp3_opnd = latxs_ra_alloc_itemp(); //shift_bit
    IR2_OPND temp4_opnd = latxs_ra_alloc_itemp(); 
    /* if the reg is 64 bits or is zero-extend from the high bits */
    lsassert(ir1_opnd_is_gpr(ir1_get_opnd(pir1, 0)));
    IR2_OPND result_opnd = latxs_ra_alloc_itemp();

    //la_mov64(temp3_opnd, zero_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_OR, &temp3_opnd, &latxs_zero_ir2_opnd, &latxs_zero_ir2_opnd);
    //la_mov64(result_opnd, zero_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_OR, &result_opnd, &latxs_zero_ir2_opnd, &latxs_zero_ir2_opnd);
    //la_beq(mask_opnd, zero_ir2_opnd, label_exit);
    latxs_append_ir2_opnd3(LISA_BEQ, &mask_opnd, &latxs_zero_ir2_opnd, &label_exit);

    //la_label(label_ctz);
    latxs_append_ir2_opnd1(LISA_LABEL, &label_ctz);
    if (ir1_opnd_size(ir1_get_opnd(pir1, 0)) == 32) {
        //la_ctz_w(temp1_opnd, mask_opnd);
        latxs_append_ir2_opnd2(LISA_CTZ_W, &temp1_opnd, &mask_opnd);
        //la_srl_w(temp4_opnd, src_opnd, temp3_opnd);
        latxs_append_ir2_opnd3(LISA_SRL_W, &temp4_opnd, &src_opnd, &temp3_opnd);
        //la_addi_w(temp2_opnd, mask_opnd, -1);
        latxs_append_ir2_opnd2i(LISA_ADDI_W, &temp2_opnd, &mask_opnd, -1);
        //la_andi(temp4_opnd, temp4_opnd, 0x1);
        latxs_append_ir2_opnd2i(LISA_ANDI, &temp4_opnd, &temp4_opnd, 1);
        //la_sll_w(temp4_opnd, temp4_opnd, temp1_opnd);
        latxs_append_ir2_opnd3(LISA_SLL_W, &temp4_opnd, &temp4_opnd, &temp1_opnd);
        //la_and(mask_opnd, mask_opnd, temp2_opnd);
        latxs_append_ir2_opnd3(LISA_AND, &mask_opnd, &mask_opnd, &temp2_opnd);
        //la_or(result_opnd, result_opnd, temp4_opnd);
        latxs_append_ir2_opnd3(LISA_OR, &result_opnd, &result_opnd, &temp4_opnd);
        //la_addi_w(temp3_opnd, temp3_opnd, 1);
        latxs_append_ir2_opnd2i(LISA_ADDI_W, &temp3_opnd, &temp3_opnd, 1);
    } else {
        //la_ctz_d(temp1_opnd, mask_opnd);
        latxs_append_ir2_opnd2(LISA_CTZ_D, &temp1_opnd, &mask_opnd);
        //la_srl_d(temp4_opnd, src_opnd, temp3_opnd);
        latxs_append_ir2_opnd3(LISA_SRL_D, &temp4_opnd, &src_opnd, &temp3_opnd);
        //la_addi_d(temp2_opnd, mask_opnd, -1);
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &temp2_opnd, &mask_opnd, -1);
        //la_andi(temp4_opnd, temp4_opnd, 0x1);
        latxs_append_ir2_opnd2i(LISA_ANDI, &temp4_opnd, &temp4_opnd, 1);
        //la_sll_d(temp4_opnd, temp4_opnd, temp1_opnd);
        latxs_append_ir2_opnd3(LISA_SLL_D, &temp4_opnd, &temp4_opnd, &temp1_opnd);
        //la_and(mask_opnd, mask_opnd, temp2_opnd);
        latxs_append_ir2_opnd3(LISA_AND, &mask_opnd, &mask_opnd, &temp2_opnd);
        //la_or(result_opnd, result_opnd, temp4_opnd);
        latxs_append_ir2_opnd3(LISA_OR, &result_opnd, &result_opnd, &temp4_opnd);
        //la_addi_d(temp3_opnd, temp3_opnd, 1);
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &temp3_opnd, &temp3_opnd, 1);
    }
    //la_bne(mask_opnd, zero_ir2_opnd, label_ctz);
    latxs_append_ir2_opnd3(LISA_BNE, &mask_opnd, &latxs_zero_ir2_opnd, &label_ctz);

    //la_label(label_exit);
    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    /* 3. store result */
    //store_ireg_to_ir1(result_opnd, ir1_get_opnd(pir1, 0), false);
    latxs_store_ir2_to_ir1(&result_opnd, ir1_get_opnd(pir1, 0));
    latxs_ra_free_temp(&result_opnd);
    return true;
}

bool latxs_translate_pext(IR1_INST *pir1)
{
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    IR2_OPND label_ctz = latxs_ir2_opnd_new_label();
    // IR2_OPND src_opnd =
    //     load_ireg_from_ir1(ir1_get_opnd(pir1, 1), ZERO_EXTENSION, false);
    IR2_OPND src_opnd = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src_opnd, ir1_get_opnd(pir1, 1), EXMode_Z);
    IR2_OPND mask_opnd = latxs_ra_alloc_itemp();
    //load_ireg_from_ir1_2(mask_opnd, ir1_get_opnd(pir1, 2), ZERO_EXTENSION, false);
    latxs_load_ir1_to_ir2(&mask_opnd, ir1_get_opnd(pir1, 2), EXMode_Z);
    IR2_OPND temp1_opnd = latxs_ra_alloc_itemp();
    IR2_OPND temp2_opnd = latxs_ra_alloc_itemp();
    IR2_OPND temp3_opnd = latxs_ra_alloc_itemp(); //shift_bit
    /* if the reg is 64 bits or is zero-extend from the high bits */
    lsassert(ir1_opnd_is_gpr(ir1_get_opnd(pir1, 0)));
    IR2_OPND result_opnd = latxs_ra_alloc_itemp();

    //la_mov64(temp3_opnd, zero_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_OR, &temp3_opnd, &latxs_zero_ir2_opnd, &latxs_zero_ir2_opnd);
    //la_mov64(result_opnd, zero_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_OR, &result_opnd, &latxs_zero_ir2_opnd, &latxs_zero_ir2_opnd);
    //la_beq(mask_opnd, zero_ir2_opnd, label_exit);
    latxs_append_ir2_opnd3(LISA_BEQ, &mask_opnd, &latxs_zero_ir2_opnd, &label_exit);

    //la_label(label_ctz);
    latxs_append_ir2_opnd1(LISA_LABEL, &label_ctz);
    if (ir1_opnd_size(ir1_get_opnd(pir1, 0)) == 32) {
        //la_ctz_w(temp1_opnd, mask_opnd);
        latxs_append_ir2_opnd2(LISA_CTZ_W, &temp1_opnd, &mask_opnd);
        //la_srl_w(temp1_opnd, src_opnd, temp1_opnd);
        latxs_append_ir2_opnd3(LISA_SRL_W, &temp1_opnd, &src_opnd, &temp1_opnd);
        //la_addi_w(temp2_opnd, mask_opnd, -1);
        latxs_append_ir2_opnd2i(LISA_ADDI_W, &temp2_opnd, &mask_opnd, -1);
        //la_andi(temp1_opnd, temp1_opnd, 0x1);
        latxs_append_ir2_opnd2i(LISA_ANDI, &temp1_opnd, &temp1_opnd, 1);
        //la_sll_w(temp1_opnd, temp1_opnd, temp3_opnd);
        latxs_append_ir2_opnd3(LISA_SLL_W, &temp1_opnd, &temp1_opnd, &temp3_opnd);
        //la_and(mask_opnd, mask_opnd, temp2_opnd);
        latxs_append_ir2_opnd3(LISA_AND, &mask_opnd, &mask_opnd, &temp2_opnd);
        //la_or(result_opnd, result_opnd, temp1_opnd);
        latxs_append_ir2_opnd3(LISA_OR, &result_opnd, &result_opnd, &temp1_opnd);
        //la_addi_w(temp3_opnd, temp3_opnd, 1);
        latxs_append_ir2_opnd2i(LISA_ADDI_W, &temp3_opnd, &temp3_opnd, 1);
    } else {
        //la_ctz_d(temp1_opnd, mask_opnd);
        latxs_append_ir2_opnd2(LISA_CTZ_D, &temp1_opnd, &mask_opnd);
        //la_srl_d(temp1_opnd, src_opnd, temp1_opnd);
        latxs_append_ir2_opnd3(LISA_SRL_D, &temp1_opnd, &src_opnd, &temp1_opnd);
        //la_addi_d(temp2_opnd, mask_opnd, -1);
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &temp2_opnd, &mask_opnd, -1);
        //la_andi(temp1_opnd, temp1_opnd, 0x1);
        latxs_append_ir2_opnd2i(LISA_ANDI, &temp1_opnd, &temp1_opnd, 1);
        //la_sll_d(temp1_opnd, temp1_opnd, temp3_opnd);
        latxs_append_ir2_opnd3(LISA_SLL_D, &temp1_opnd, &temp1_opnd, &temp3_opnd);
        //la_and(mask_opnd, mask_opnd, temp2_opnd);
        latxs_append_ir2_opnd3(LISA_AND, &mask_opnd, &mask_opnd, &temp2_opnd);
        //la_or(result_opnd, result_opnd, temp1_opnd);
        latxs_append_ir2_opnd3(LISA_OR, &result_opnd, &result_opnd, &temp1_opnd);
        //la_addi_d(temp3_opnd, temp3_opnd, 1);
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &temp3_opnd, &temp3_opnd, 1);
    }
    //la_bne(mask_opnd, zero_ir2_opnd, label_ctz);
    latxs_append_ir2_opnd3(LISA_BNE, &mask_opnd, &latxs_zero_ir2_opnd, &label_ctz);

    //la_label(label_exit);
    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    /* 3. store result */
    //store_ireg_to_ir1(result_opnd, ir1_get_opnd(pir1, 0), false);
    latxs_store_ir2_to_ir1(&result_opnd, ir1_get_opnd(pir1, 0));
    latxs_ra_free_temp(&result_opnd);
    return true;
}

bool latxs_translate_salc(IR1_INST *pir1)
{
    IR2_OPND label_1 = latxs_ir2_opnd_new_label();
    IR2_OPND label_2 = latxs_ir2_opnd_new_label();
    // IR2_OPND src_opnd =
    //     load_ireg_from_ir1(&eax_ir1_opnd, ZERO_EXTENSION, false);
    IR2_OPND src_opnd = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src_opnd, &eax_ir1_opnd, EXMode_Z);
    IR2_OPND flag_opnd = latxs_ra_alloc_itemp();
    //la_x86mfflag(flag_opnd, CF_USEDEF_BIT);
    latxs_append_ir2_opnd1i(LISA_X86MFFLAG, &flag_opnd,  CF_USEDEF_BIT);

    //la_bne(flag_opnd, zero_ir2_opnd, label_1);
    latxs_append_ir2_opnd3(LISA_BNE, &flag_opnd, &latxs_zero_ir2_opnd, &label_1);
    //la_bstrins_w(src_opnd, zero_ir2_opnd, 7, 0);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &src_opnd, &latxs_zero_ir2_opnd, 7, 0);
    //la_b(label_2);
    latxs_append_ir2_opnd1(LISA_B, &label_2);
    //la_label(label_1);
    latxs_append_ir2_opnd1(LISA_LABEL, &label_1);
    //la_ori(src_opnd, src_opnd, 0xff);
    latxs_append_ir2_opnd2i(LISA_ORI, &src_opnd, &src_opnd, 0xff);
    //la_label(label_2);
    latxs_append_ir2_opnd1(LISA_LABEL, &label_2);
    //store_ireg_to_ir1(src_opnd, &eax_ir1_opnd, false);
    latxs_store_ir2_to_ir1(&src_opnd, &eax_ir1_opnd);
    return true;
}
