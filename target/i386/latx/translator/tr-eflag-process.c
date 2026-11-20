/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "reg-alloc.h"
#include "lsenv.h"
#include "latx-options.h"
#include "flag-lbt.h"
#include "translate.h"
#include "latx-sys-hamt-dmw.h"
#include "sys-aot.h"

static void latxs_generate_cf(
        IR2_OPND *dest, IR2_OPND *src0, IR2_OPND *src1)
{
    IR1_INST *pir1 = lsenv->tr_data->curr_ir1_inst;

    switch (ir1_opcode(pir1)) {
    case X86_INS_SHLD: {
        int mask = ((ir1_opnd_size(ir1_get_opnd(pir1, 0)) == 64) ? 0x3f : 0x1f);
        if (latxs_ir2_opnd_is_imm(src1)) {
            lsassertm((latxs_ir2_opnd_imm(src1) & mask) == latxs_ir2_opnd_imm(src1),
                        "The value cannot be 0x%"PRIx16, latxs_ir2_opnd_imm(src1));
            IR2_OPND t_dest_opnd = latxs_ra_alloc_itemp();
            int count = latxs_ir2_opnd_imm(src1);
            latxs_append_ir2_opnd2i(LISA_SRLI_D, &t_dest_opnd, src0, mask + 1 - count);

            latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &t_dest_opnd, 0x1);
            latxs_ra_free_temp(&t_dest_opnd);
        } else {

            IR2_OPND t_dest_opnd = latxs_ra_alloc_itemp();
            /* src0 >> (size - count) */
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &t_dest_opnd, &latxs_zero_ir2_opnd, mask + 1);
            latxs_append_ir2_opnd3(LISA_SUB_D, &t_dest_opnd, &t_dest_opnd, src1);
            latxs_append_ir2_opnd3(LISA_SRL_D, &t_dest_opnd, src0, &t_dest_opnd);

            latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &t_dest_opnd, 0x1);
            latxs_ra_free_temp(&t_dest_opnd);
        }
        return;
    }
    case X86_INS_SHRD: {
        if (latxs_ir2_opnd_is_imm(src1)) {
            IR2_OPND t_dest = latxs_ra_alloc_itemp();
            int count = latxs_ir2_opnd_imm(src1) - 1;
            latxs_append_ir2_opnd2i(LISA_SRLI_D, &t_dest, src0, count);
            latxs_append_ir2_opnd2i(LISA_ANDI,   &t_dest, &t_dest, 1);
            latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &t_dest, 0x1);
            latxs_ra_free_temp(&t_dest);
        } else {
            IR2_OPND t_dest = latxs_ra_alloc_itemp();
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &t_dest, src1, -1);
            latxs_append_ir2_opnd3(LISA_SRL_D, &t_dest, src0, &t_dest);
            latxs_append_ir2_opnd2i(LISA_ANDI, &t_dest, &t_dest, 1);
            latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &t_dest, 0x1);
            latxs_ra_free_temp(&t_dest);
        }
        return;
        if (latxs_ir2_opnd_is_imm(src1)) {
            lsassertm((latxs_ir2_opnd_imm(src1) &
                ((ir1_opnd_size(ir1_get_opnd(pir1, 0)) == 64) ? 0x3f : 0x1f))
                == latxs_ir2_opnd_imm(src1), "The value cannot be 0x%"PRIx16, latxs_ir2_opnd_imm(src1));
            IR2_OPND t_dest_opnd = latxs_ra_alloc_itemp();
            int count = latxs_ir2_opnd_imm(src1) - 1;
            latxs_append_ir2_opnd2i(LISA_SRLI_D, &t_dest_opnd, src0, count);

            latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &t_dest_opnd, 0x1);
            latxs_ra_free_temp(&t_dest_opnd);
        } else {
            IR2_OPND t_dest_opnd = latxs_ra_alloc_itemp();
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &t_dest_opnd, src1, -1);
            latxs_append_ir2_opnd3(LISA_SRL_D, &t_dest_opnd, src0, &t_dest_opnd);

            latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &t_dest_opnd, 0x1);
            latxs_ra_free_temp(&t_dest_opnd);
        }
        return;
    }
    case X86_INS_BLSI: {
        IR2_OPND cf = latxs_ra_alloc_itemp();
        IR2_OPND tmp = latxs_ra_alloc_itemp();
        int opnd_size = ir1_opnd_size(ir1_get_opnd(pir1, 0));
        latxs_append_ir2_opnd2i(LISA_ORI, &tmp, &latxs_zero_ir2_opnd, 0xfff);
        if (opnd_size == 64) {
            latxs_append_ir2_opnd3(LISA_MASKEQZ, &cf, &tmp, src1);
        } else {
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &cf, src1, opnd_size - 1, 0);
            latxs_append_ir2_opnd3(LISA_MASKEQZ, &cf, &tmp, &cf);
        }
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &cf, CF_USEDEF_BIT);

        latxs_ra_free_temp(&cf);
        latxs_ra_free_temp(&tmp);
        return;
    }
    case X86_INS_BLSR:
    case X86_INS_BLSMSK: {
        IR2_OPND cf = latxs_ra_alloc_itemp();
        IR2_OPND tmp = latxs_ra_alloc_itemp();
        int opnd_size = ir1_opnd_size(ir1_get_opnd(pir1, 0));
        latxs_append_ir2_opnd2i(LISA_ORI, &tmp, &latxs_zero_ir2_opnd, 0xfff);
        if (opnd_size == 64) {
            latxs_append_ir2_opnd3(LISA_MASKNEZ, &cf, &tmp, src1);
        } else {
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &cf, src1, opnd_size - 1, 0);
            latxs_append_ir2_opnd3(LISA_MASKNEZ, &cf, &tmp, &cf);
        }
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &cf, CF_USEDEF_BIT);

        latxs_ra_free_temp(&cf);
        latxs_ra_free_temp(&tmp);
        return;
    }
    case X86_INS_LZCNT: {
        IR2_OPND label_temp = latxs_ir2_opnd_new_label();
        IR2_OPND label_exit = latxs_ir2_opnd_new_label();
        IR2_OPND temp_opnd = latxs_ra_alloc_itemp();
        int opnd_size = ir1_opnd_size(ir1_get_opnd(pir1, 0));
        //li_d(temp_opnd, opnd_size);
        latxs_load_imm64_to_ir2(&temp_opnd, opnd_size);
        //la_sub_d(temp_opnd, dest, temp_opnd);
        latxs_append_ir2_opnd3(LISA_SUB_D, &temp_opnd, dest, &temp_opnd);
        //la_beq(temp_opnd, zero_ir2_opnd, label_temp);
        latxs_append_ir2_opnd3(LISA_BEQ, &temp_opnd, &latxs_zero_ir2_opnd, &label_temp);
        //la_x86mtflag(zero_ir2_opnd, 0x1);
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_zero_ir2_opnd, 0x1);
        //la_b(label_exit);
        latxs_append_ir2_opnd1(LISA_B, &label_exit);
        //la_label(label_temp);
        latxs_append_ir2_opnd1(LISA_LABEL, &label_temp);
        //la_orn(temp_opnd, zero_ir2_opnd, zero_ir2_opnd);
        latxs_append_ir2_opnd3(LISA_ORN, &temp_opnd, &latxs_zero_ir2_opnd, &latxs_zero_ir2_opnd);
        //la_x86mtflag(temp_opnd, 0x1);
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &temp_opnd, 0x1);
        //la_label(label_exit);
        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
        return;
    }
    default:
        break;
    }

    lsassertm(0, "%s for %s is not implemented\n",
            __func__, ir1_name(ir1_opcode(pir1)));
}

char pf_table[256] = {
    4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4, 0, 4, 4, 0, 4, 0,
    0, 4, 4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4,
    0, 4, 4, 0, 4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,

    0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0, 4, 0, 0, 4, 0, 4,
    4, 0, 0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0,
    4, 0, 0, 4, 0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,

    0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0, 4, 0, 0, 4, 0, 4,
    4, 0, 0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0,
    4, 0, 0, 4, 0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4, 0, 4, 4, 0,

    4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4, 0, 4, 4, 0, 4, 0,
    0, 4, 4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4, 4, 0, 0, 4,
    0, 4, 4, 0, 4, 0, 0, 4, 0, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 4,
};

static void latxs_generate_pf(
        IR2_OPND *dest, IR2_OPND *src0, IR2_OPND *src1)
{
    IR2_OPND pf_opnd = latxs_ra_alloc_itemp();
    IR2_OPND low_byte = latxs_ra_alloc_itemp();
    latxs_load_addr_to_ir2(&pf_opnd, (ADDR)pf_table, PF_TABLE);
    latxs_append_ir2_opnd2i(LISA_ANDI, &low_byte, dest, 0xff);
    latxs_append_ir2_opnd3(LISA_ADD_D, &low_byte, &pf_opnd, &low_byte);
    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_mem(&low_byte);
    }
    latxs_append_ir2_opnd2i(LISA_LD_BU, &pf_opnd, &low_byte, 0);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &pf_opnd, 0x2);
    latxs_ra_free_temp(&pf_opnd);
    latxs_ra_free_temp(&low_byte);
}

static void latxs_generate_af(
        IR2_OPND *dest, IR2_OPND *src0, IR2_OPND *src1)
{
    IR2_OPND af_opnd = latxs_ra_alloc_itemp();
    if (latxs_ir2_opnd_is_imm(src1)) {
        latxs_append_ir2_opnd2i(LISA_XORI,
                &af_opnd, src0, latxs_ir2_opnd_imm(src1));
    } else {
        latxs_append_ir2_opnd3(LISA_XOR, &af_opnd, src0, src1);
    }
    latxs_append_ir2_opnd3(LISA_XOR, &af_opnd, &af_opnd, dest);
    latxs_append_ir2_opnd2i(LISA_ANDI, &af_opnd, &af_opnd, 0x10);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &af_opnd, 0x4);
    latxs_ra_free_temp(&af_opnd);
}

static void latxs_generate_zf(IR2_OPND *dest, IR2_OPND *src0, IR2_OPND *src1)
{
    IR1_INST *pir1 = lsenv->tr_data->curr_ir1_inst;
    int opnd_size = ir1_opnd_size(ir1_get_opnd(pir1, 0));
    lsassert(opnd_size == 8 || opnd_size == 16 || opnd_size == 32 ||
             opnd_size == 64);

    IR2_OPND extended_dest_opnd = latxs_ra_alloc_itemp();
    switch (opnd_size) {
    case 8:
        latxs_append_ir2_opnd2_(lisa_mov8z,  &extended_dest_opnd, dest);
        break;
    case 16:
        latxs_append_ir2_opnd2_(lisa_mov16z, &extended_dest_opnd, dest);
        break;
    case 32:
        latxs_append_ir2_opnd2_(lisa_mov32z, &extended_dest_opnd, dest);
        break;
    case 64:
        latxs_append_ir2_opnd2_(lisa_mov, &extended_dest_opnd, dest);
        break;
    default:
        lsassert(0);
        break;
    }

    IR2_OPND temp_eflags = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ORI, &temp_eflags,
            &latxs_zero_ir2_opnd, 0xfff);

    IR2_OPND is_zero = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &extended_dest_opnd,
            &latxs_zero_ir2_opnd, &is_zero);
    latxs_append_ir2_opnd2_(lisa_mov, &temp_eflags, &latxs_zero_ir2_opnd);
    latxs_append_ir2_opnd1(LISA_LABEL, &is_zero);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &temp_eflags, 0x8);
    latxs_ra_free_temp(&temp_eflags);
}

static void latxs_generate_sf(IR2_OPND *dest, IR2_OPND *src0, IR2_OPND *src1)
{
    IR1_INST *pir1 = lsenv->tr_data->curr_ir1_inst;
    int os = ir1_opnd_size(ir1_get_opnd(pir1, 0));
    IR2_OPND sf_opnd = latxs_ra_alloc_itemp();

    if (os > 8) {
        latxs_append_ir2_opnd2i(LISA_SRLI_D, &sf_opnd, dest, os - 8);
        latxs_append_ir2_opnd2i(LISA_ANDI, &sf_opnd, &sf_opnd, SF_BIT);
    } else {
        latxs_append_ir2_opnd2i(LISA_ANDI, &sf_opnd, dest, SF_BIT);
    }

    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &sf_opnd, 0x10);
    latxs_ra_free_temp(&sf_opnd);
}

static void latxs_generate_of(IR2_OPND *dest, IR2_OPND *src0, IR2_OPND *src1)
{
    IR1_INST *pir1 = lsenv->tr_data->curr_ir1_inst;

    switch (ir1_opcode(pir1)) {
    case X86_INS_SHLD:{
        IR2_OPND count_opnd = latxs_ra_alloc_itemp();
        if (latxs_ir2_opnd_is_imm(src1)) {
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &count_opnd, &latxs_zero_ir2_opnd,
                latxs_ir2_opnd_imm(src1));
        } else {
            latxs_append_ir2_opnd3(LISA_ADD_D, &count_opnd, &latxs_zero_ir2_opnd, src1);
        }
        IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
        IR2_OPND dest_old;
        int opnd_size = ir1_opnd_size(opnd1);
        if (opnd_size == 16) {
            IR2_OPND label_temp = latxs_ir2_opnd_new_label();
            IR2_OPND label_exit = latxs_ir2_opnd_new_label();
            IR2_OPND size = latxs_ra_alloc_itemp();
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &size, &latxs_zero_ir2_opnd, opnd_size + 1);
            latxs_append_ir2_opnd3(LISA_BGE, &count_opnd, &size, &label_temp);
            latxs_ra_free_temp(&size);

            dest_old = latxs_ra_alloc_itemp();
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &dest_old, src0, 31, 16);
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &count_opnd, &count_opnd, -1);
            latxs_append_ir2_opnd3(LISA_SLL_D, &dest_old, &dest_old, &count_opnd);

            latxs_append_ir2_opnd1(LISA_B, &label_exit);

            latxs_append_ir2_opnd1(LISA_LABEL, &label_temp);

            latxs_load_ir1_to_ir2(&dest_old, opnd1, EXMode_Z);
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &count_opnd, &count_opnd, -17);
            latxs_append_ir2_opnd3(LISA_SLL_D, &dest_old, &dest_old, &count_opnd);

            latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

        } else {
            dest_old = latxs_ra_alloc_itemp();
            latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &dest_old, src0, 31, 16);
            latxs_append_ir2_opnd3(LISA_ADD_D, &dest_old, &latxs_zero_ir2_opnd, src0);

            latxs_append_ir2_opnd2i(LISA_ADDI_D, &count_opnd, &count_opnd, -1);
            latxs_append_ir2_opnd3(LISA_SLL_D, &dest_old, &dest_old, &count_opnd);
        }

        latxs_ra_free_temp(&count_opnd);
        /*
         * In x86 cpu, the undefined OF when shifting bits > 1
         * is designed the same behavior with shifting bit == 1
         */
        IR2_OPND t_of_opnd = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd3(LISA_XOR, &t_of_opnd, &dest_old, dest);
        latxs_append_ir2_opnd2i(LISA_SRLI_D, &t_of_opnd, &t_of_opnd,
                ir1_opnd_size(ir1_get_opnd(pir1, 0)) - OF_BIT_INDEX - 1);

        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &t_of_opnd, 0x20);
        latxs_ra_free_temp(&dest_old);
        latxs_ra_free_temp(&t_of_opnd);


        return;
    }
    case X86_INS_SHRD: {
        IR2_OPND count_opnd = latxs_ra_alloc_itemp();
        if (latxs_ir2_opnd_is_imm(src1)) {
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &count_opnd, &latxs_zero_ir2_opnd,
                latxs_ir2_opnd_imm(src1));
        } else {
            latxs_append_ir2_opnd3(LISA_ADD_D, &count_opnd, &latxs_zero_ir2_opnd, src1);
        }
        IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
        int opnd_size = ir1_opnd_size(opnd0);
        IR2_OPND dest_old = latxs_ra_alloc_itemp();

        if (opnd_size == 16) {
            latxs_append_ir2_opnd3(LISA_OR, &dest_old, &latxs_zero_ir2_opnd, src0);
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &dest_old, &dest_old, 47, 32);

            latxs_append_ir2_opnd2i(LISA_ADDI_D, &count_opnd, &count_opnd, -1);
            latxs_append_ir2_opnd3(LISA_SRL_D, &dest_old, &dest_old, &count_opnd);
        } else {
            IR2_OPND label_exit = latxs_ir2_opnd_new_label();
            latxs_append_ir2_opnd3(LISA_BEQ, &count_opnd, &latxs_zero_ir2_opnd, &label_exit);
            latxs_append_ir2_opnd2i(LISA_SLLI_D, &dest_old, dest, 1);
            latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
        }
        latxs_ra_free_temp(&count_opnd);

        /*
         * In x86 cpu, the undefined OF when shifting bits > 1
         * is designed the same behavior with shifting bit == 1
         */
        IR2_OPND t_of_opnd = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd3(LISA_XOR, &t_of_opnd, &dest_old, dest);
        latxs_append_ir2_opnd2i(LISA_SRLI_D, &t_of_opnd, &t_of_opnd,
                ir1_opnd_size(ir1_get_opnd(pir1, 0)) - OF_BIT_INDEX - 1);

        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &t_of_opnd, 0x20);
        latxs_ra_free_temp(&dest_old);
        latxs_ra_free_temp(&t_of_opnd);
        return;
    }
    default:
        break;
    }

    lsassertm(0, "%s for %s is not implemented\n",
            __func__, ir1_name(ir1_opcode(pir1)));
}

static void latxs_generate_cf_not_sx(
        IR2_OPND *dest, IR2_OPND *src0, IR2_OPND *src1)
{
    lsassertm(0, "%s to be implemented in LoongArch.\n", __func__);
}

static void latxs_generate_of_not_sx(
        IR2_OPND *dest, IR2_OPND *src0, IR2_OPND *src1)
{
    lsassertm(0, "%s to be implemented in LoongArch.\n", __func__);
}

void latxs_generate_eflag_calculation(
        IR2_OPND *dest, IR2_OPND *src0, IR2_OPND *src1,
        IR1_INST *pir1, bool is_sx)
{
    if (ir1_get_eflag_def(pir1) == 0 ||
        ir1_need_calculate_any_flag(pir1) == 0) {
        return;
    }

    if (latxs_generate_eflag_by_lbt(dest, src0, src1, pir1, is_sx)) {
        if (ir1_opcode(pir1) == X86_INS_BLSI ||
            ir1_opcode(pir1) == X86_INS_BLSR ||
            ir1_opcode(pir1) == X86_INS_BLSMSK) {
            latxs_generate_cf(dest, src0, src1);
        } else if (ir1_opcode(pir1) == X86_INS_SBB) {
            latxs_generate_af(dest, src0, src1);
        }
        return;
    }

    /* extension mode does not affect pf, af and zf */
    if (ir1_need_calculate_pf(pir1)) {
        latxs_generate_pf(dest, src0, src1);
    }
    if (ir1_need_calculate_af(pir1)) {
        latxs_generate_af(dest, src0, src1);
    }
    if (ir1_need_calculate_zf(pir1)) {
        latxs_generate_zf(dest, src0, src1);
    }
    if (ir1_need_calculate_sf(pir1)) {
        latxs_generate_sf(dest, src0, src1);
    }

    /* calculate cf and of separately */
    if (is_sx) {
        if (ir1_need_calculate_cf(pir1)) {
            latxs_generate_cf(dest, src0, src1);
        }
        if (ir1_need_calculate_of(pir1)) {
            latxs_generate_of(dest, src0, src1);
        }
    } else {
        if (ir1_need_calculate_cf(pir1)) {
            latxs_generate_cf_not_sx(dest, src0, src1);
        }
        if (ir1_need_calculate_of(pir1)) {
            latxs_generate_of_not_sx(dest, src0, src1);
        }
    }
}
void latxs_generate_imul_eflag_calculation(
        IR2_OPND *dest, IR2_OPND *src0, IR2_OPND *src1,
        IR1_INST *pir1, bool is_sx)
{
    if (ir1_get_eflag_def(pir1) == 0 ||
        ir1_need_calculate_any_flag(pir1) == 0) {
        return;
    }

    /* extension mode does not affect pf, af and zf */
    if (ir1_need_calculate_pf(pir1)) {
        latxs_generate_pf(dest, src0, src1);
    }
    if (ir1_need_calculate_af(pir1)) {
        latxs_generate_af(dest, src0, src1);
    }
    if (ir1_need_calculate_zf(pir1)) {
        latxs_generate_zf(dest, src0, src1);
    }
    if (ir1_need_calculate_sf(pir1)) {
        latxs_generate_sf(dest, src0, src1);
    }

    /* calculate cf and of separately */
    if (is_sx) {
        if (ir1_need_calculate_cf(pir1)) {
            latxs_generate_cf(dest, src0, src1);
        }
        if (ir1_need_calculate_of(pir1)) {
            latxs_generate_of(dest, src0, src1);
        }
    } else {
        if (ir1_need_calculate_cf(pir1)) {
            latxs_generate_cf_not_sx(dest, src0, src1);
        }
        if (ir1_need_calculate_of(pir1)) {
            latxs_generate_of_not_sx(dest, src0, src1);
        }
    }
}
