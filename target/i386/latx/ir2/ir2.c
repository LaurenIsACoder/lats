/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "../include/common.h"
#include "../include/ir1.h"
#include "../include/lsenv.h"
#include "../include/reg-alloc.h"
#include "../include/translate.h"
#include <string.h>

#include "../include/ir2.h"
#include "hamt-options.h"
#include "latx-options.h"
#include "latx-sys-hamt-dmw.h"
#include "sys-aot-format.h"

const char *latxs_ir2_name(int value)
{
    /*
     * Todo: Add below defination:
     */
    #if 0
    static const char *ir2_scr_name[] = {
        "$scr0" , "$scr1" , "$scr2" , "$scr3",
    };

    static const char *ir2_cc_name[] = {
        "$cc0" , "$cc1" , "$cc2" , "$cc3" ,
        "$cc4" , "$cc5" , "$cc6" , "$cc7" ,
    };
    #endif
    static const char * const g_ir2_names[] = {
        "$zero" , "$ra" , "$tp" , "$sp" , "$a0" , "$a1" , "$a2" , "$a3" ,
        "$a4"   , "$a5" , "$a6" , "$a7" , "$t0" , "$t1" , "$t2" , "$t3" ,
        "$t4"   , "$t5" , "$t6" , "$t7" , "$t8" , "$x"  , "$fp" , "$s0" ,
        "$s1"   , "$s2" , "$s3" , "$s4" , "$s5" , "$s6" , "$s7" , "$s8" ,
        "", "", "", "", "", "", "", "", /*  32-39 */
        "$fa0"  , "$fa1"  , "$fa2"  , "$fa3"  ,
        "$fa4"  , "$fa5"  , "$fa6"  , "$fa7"  ,
        "$ft0"  , "$ft1"  , "$ft2"  , "$ft3"  ,
        "$ft4"  , "$ft5"  , "$ft6"  , "$ft7"  ,
        "$ft8"  , "$ft9"  , "$ft10" , "$ft11" ,
        "$ft12" , "$ft13" , "$ft14" , "$ft15" ,
        "$fs0"  , "$fs1"  , "$fs2"  , "$fs3"  ,
        "$fs4"  , "$fs5"  , "$fs6"  , "$s7"   ,
        "", "", "", "", "", "", "", "", /* 72-79 */
        "NONE", "GPR", "SCR", "FPR", "FCSR", "IMMD", "IMMH", "LABEL",/* 80-87 */
        "", "", "", "", "", "", "", "", /* 88-95 */
        "ax", "sx", "zx", "bx", "AD", "AX", "", "",  /* 96-103 */
        "", "", "", "", "", "", "", "", /* 104-111 */
        "", "", "", "", "", "", "", "", /* 112-119 */
        "", "", "", "", "", "", "", "", /* 120-127 */

#include "../include/ir2-name.h"

    };
    lsassert(value <= LISA_XVFCMP_COND_Q);
    return g_ir2_names[value];
}

#define LATXS_REG_ALLOC_DEFINE_IR2(ir2name, ir2type, ir2val) \
IR2_OPND ir2name = {._type = ir2type, .val = ir2val}

#define LATXS_REG_ALLOC_DEFINE_IR2_GPR(ir2name, ir2val) \
LATXS_REG_ALLOC_DEFINE_IR2(ir2name, IR2_OPND_GPR, ir2val);

#define LATXS_REG_ALLOC_DEFINE_IR2_CSR(ir2name, ir2val) \
LATXS_REG_ALLOC_DEFINE_IR2(ir2name, IR2_OPND_CSR, ir2val);

/* Integer Register */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_zero_ir2_opnd,    0); /* ZERO */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_sp_ir2_opnd,      3); /* SP */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_fp_ir2_opnd,     22); /* FP */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_env_ir2_opnd,    23); /* S0 */

/* Floating Point  Register */
LATXS_REG_ALLOC_DEFINE_IR2(latxs_f32_ir2_opnd, IR2_OPND_FPR, 8);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcsr_ir2_opnd, IR2_OPND_FCSR, 0);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcsr1_ir2_opnd, IR2_OPND_FCSR, 1);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcsr2_ir2_opnd, IR2_OPND_FCSR, 2);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcsr3_ir2_opnd, IR2_OPND_FCSR, 3);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcc0_ir2_opnd, IR2_OPND_CC, 0);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcc1_ir2_opnd, IR2_OPND_CC, 1);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcc2_ir2_opnd, IR2_OPND_CC, 2);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcc3_ir2_opnd, IR2_OPND_CC, 3);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcc4_ir2_opnd, IR2_OPND_CC, 4);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcc5_ir2_opnd, IR2_OPND_CC, 5);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcc6_ir2_opnd, IR2_OPND_CC, 6);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_fcc7_ir2_opnd, IR2_OPND_CC, 7);

/* Helper Arguments Register */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_arg0_ir2_opnd,  4); /* a0 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_arg1_ir2_opnd,  5); /* a1 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_arg2_ir2_opnd,  6); /* a2 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_arg3_ir2_opnd,  7); /* a3 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_arg4_ir2_opnd,  8); /* a4/stmp1      */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_arg5_ir2_opnd,  9); /* a5/stmp2      */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_arg6_ir2_opnd, 10); /* a6/TB address */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_arg7_ir2_opnd, 11); /* a7/eip        */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_ret0_ir2_opnd,  4); /* v0 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_ret1_ir2_opnd,  5); /* v1 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_ra_ir2_opnd,    1); /* ra */

/* Static Temp Registers */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_stmp1_ir2_opnd, 8); /* a4 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_stmp2_ir2_opnd, 9); /* a5 */

/* LA temp registers */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_tmp0_ir2_opnd, 12); /* t0 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_tmp1_ir2_opnd, 13); /* t1 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_tmp2_ir2_opnd, 14); /* t2 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_tmp3_ir2_opnd, 15); /* t3 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_tmp4_ir2_opnd, 16); /* t4 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_tmp5_ir2_opnd, 17); /* t5 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_tmp6_ir2_opnd, 18); /* t6 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_tmp7_ir2_opnd, 19); /* t7 */

/* LA save registers */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_s0_ir2_opnd, 23); /* s0 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_s1_ir2_opnd, 24); /* s1 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_s2_ir2_opnd, 25); /* s2 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_s3_ir2_opnd, 26); /* s3 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_s4_ir2_opnd, 27); /* s4 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_s5_ir2_opnd, 28); /* s5 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_s6_ir2_opnd, 29); /* s6 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_s7_ir2_opnd, 30); /* s7 */
LATXS_REG_ALLOC_DEFINE_IR2_GPR(latxs_s8_ir2_opnd, 31); /* s8 */

/* Consist value */
LATXS_REG_ALLOC_DEFINE_IR2(latxs_invalid_ir2_opnd, IR2_OPND_INV, 0);

LATXS_REG_ALLOC_DEFINE_IR2(latxs_scr0_ir2_opnd, IR2_OPND_SCR, 0);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_scr1_ir2_opnd, IR2_OPND_SCR, 1);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_scr2_ir2_opnd, IR2_OPND_SCR, 2);
LATXS_REG_ALLOC_DEFINE_IR2(latxs_scr3_ir2_opnd, IR2_OPND_SCR, 3);

/* CSR registers */
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_crmd,   0x0);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_prmd,   0x1);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_euen,   0x2);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_misc,   0x3);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_ecfg,   0x4);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_estat,  0x5);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_era,    0x6);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_badv,   0x7);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_badi,   0x8);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_eentry, 0xc);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_rvacfg, 0x1f);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_cpuid,  0x20);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_prcfg1, 0x21);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_prcfg2, 0x22);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_prcfg3, 0x23);

LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbidx,  0x10);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbehi,  0x11);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbelo0, 0x12);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbelo1, 0x13);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_asid,    0x18);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_stlbps,  0x1e);

LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_ks0,  0x30);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_ks1,  0x31);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_ks2,  0x32);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_ks3,  0x33);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_ks4,  0x34);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_ks5,  0x35);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_ks6,  0x36);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_ks7,  0x37);

LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tcfg,  0x41);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tintclear,  0x44);

LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_impctl1 , 0x80);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_impctl2 , 0x81);

LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbr_entry, 0x88);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbr_badv,  0x89);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbr_era,   0x8a);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbr_save,  0x8b);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbr_elo0,  0x8c);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbr_elo1,  0x8d);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbr_ehi,   0x8e);
LATXS_REG_ALLOC_DEFINE_IR2_CSR(la_csr_tlbr_prmd,  0x8f);

/* Functions to convert IR2_INST to string */
int latxs_ir2_dump(IR2_INST *ir2)
{
    char str[64];
    int size = 0;

    if (ir2_opcode(ir2) == 0) {
        /*
         * an empty IR2_INST was inserted into the ir2 list,
         * but not assigned yet.
         */
        return 0;
    }

    size = latxs_ir2_to_string(ir2, str);
    fprintf(stderr, "[%03d] %s", ir2->_id, str);
    if (ir2->_b_label_id) {
        fprintf(stderr, "\t ------> LABEL %d at [%03d]",
                ir2->_b_label_id, ir2->_b_ir2_id);
    }
#if defined(CONFIG_LATX_IR2_COMMENT)
    if (ir2->comment) {
        fprintf(stderr, " \t : %s", ir2->comment);
    }
#endif
    fprintf(stderr, "\n");
    return size;
}

int latxs_ir2_to_string(IR2_INST *ir2, char *str)
{
    int length = 0;
    int i = 0;
    uint32_t hexmask = 0xffffffff;

    length = sprintf(str, "%-8s  ", latxs_ir2_name(ir2_opcode(ir2)));

    switch (ir2_opcode(ir2)) {
    case LISA_ANDI:
    case LISA_ORI:
    case LISA_XORI:
    case LISA_LU52I_D:
        hexmask = 0xfff;
        break;
    case LISA_LU12I_W:
    case LISA_LU32I_D:
        hexmask = 0xfffff;
        break;
    default:
        break;
    }

    for (i = 0; i < 4; ++i) {
        IR2_OPND *opnd = &ir2->_opnd[i];
        if (latxs_ir2_opnd_type(opnd) == IR2_OPND_INV) {
            return length;
        } else {
            if (i > 0) {
                strcat(str, ",");
                length += 1;
            }
            length += latxs_ir2_opnd_to_string(opnd, str + length, hexmask);
        }
    }

    return length;
}

int latxs_ir2_opnd_to_string(IR2_OPND *opnd, char *str, uint32_t hexmask)
{
    int reg_num = latxs_ir2_opnd_reg(opnd);
    int imm = 0;

    switch (latxs_ir2_opnd_type(opnd)) {
    case IR2_OPND_INV: return 0;
    case IR2_OPND_GPR: {
        strcpy(str, latxs_ir2_name(reg_num));
        return strlen(str);
    }
    case IR2_OPND_FPR: {
        strcpy(str, latxs_ir2_name(40 + reg_num));
        return strlen(str);
    }
    case IR2_OPND_SCR: {
        return sprintf(str, "$scr%d", reg_num);
    }
    case IR2_OPND_FCSR: {
        return sprintf(str, "$c%d", reg_num);
    }
    case IR2_OPND_CC: {
        return sprintf(str, "$c%d", reg_num);
    }
    case IR2_OPND_IMMD: {
        return sprintf(str, "%d", latxs_ir2_opnd_imm(opnd));
    }
    case IR2_OPND_IMMH: {
        imm = latxs_ir2_opnd_imm(opnd);
        return sprintf(str, "%d(0x%x)", imm, (uint32_t)(imm & hexmask));
    }
    case IR2_OPND_LABEL:
        return sprintf(str, "LABEL %d", latxs_ir2_opnd_imm(opnd));
    case IR2_OPND_CSR: 
        return sprintf(str, "csr(0x%x)", latxs_ir2_opnd_imm(opnd));
    case IR2_OPND_CODE: 
        return sprintf(str, "code(0x%x)", latxs_ir2_opnd_imm(opnd));
    default:
        lsassertm(0, "type = %d\n", latxs_ir2_opnd_type(opnd));
        return 0;
    }
}

IR2_INST *latxs_append_ir2_opnd3i(IR2_OPCODE opcode,
        IR2_OPND *opnd0, IR2_OPND *opnd1,
        IR2_OPND *opnd2, int imm)
{
    IR2_INST *pir2 = ir2_allocate();

    IR2_OPND imm_opnd = latxs_ir2_opnd_new(IR2_OPND_IMMH, imm);
    latxs_ir2_build4(pir2, opcode, opnd0, opnd1, opnd2, &imm_opnd);

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));

    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_ir2_opnd1ii(IR2_OPCODE opcode,
        IR2_OPND *opnd0, int imm1, int imm2)
{
    IR2_INST *pir2 = ir2_allocate();
    IR2_OPND ir2_opnd1, ir2_opnd2;
    latxs_ir2_opnd_build(&ir2_opnd1, IR2_OPND_IMMH, imm1);
    latxs_ir2_opnd_build(&ir2_opnd2, IR2_OPND_IMMH, imm2);

    switch (opcode) {
    case LISA_VMEPATMSK_V:
    case LISA_XVMEPATMSK_V:
        latxs_ir2_build3(pir2, opcode,
                opnd0, &ir2_opnd1, &ir2_opnd2);
        break;
    default:
        lsassertm(0, "append opnd1ii unsupport LISA instruciton (%d)%s\n",
                opcode, latxs_ir2_name(opcode));
        break;
    }

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));

    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_ir2_opnd2ii(IR2_OPCODE opcode,
        IR2_OPND *opnd0, IR2_OPND *opnd1,
        int imm1, int imm2)
{
    IR2_INST *pir2 = ir2_allocate();
    IR2_OPND ir2_opnd2, ir2_opnd3;
    latxs_ir2_opnd_build(&ir2_opnd2, IR2_OPND_IMMH, imm1);
    latxs_ir2_opnd_build(&ir2_opnd3, IR2_OPND_IMMH, imm2);

    switch (opcode) {
    case LISA_BSTRINS_W:
    case LISA_BSTRINS_D:
    case LISA_BSTRPICK_W:
    case LISA_BSTRPICK_D:
    case LISA_VSTELM_D:
        latxs_ir2_build4(pir2, opcode,
                opnd0, opnd1, &ir2_opnd2, &ir2_opnd3);
        break;
    default:
        lsassertm(0, "append opnd2ii unsupport LISA instruciton (%d)%s\n",
                opcode, latxs_ir2_name(opcode));
        break;
    }

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));

    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_ir2_opnd3(IR2_OPCODE opcode,
        IR2_OPND  *opnd0, IR2_OPND *opnd1, IR2_OPND *opnd2)
{
    IR2_INST *pir2 = ir2_allocate();
    latxs_ir2_build3(pir2, opcode, opnd0, opnd1, opnd2);

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));

    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_ir2_opnd4(IR2_OPCODE opcode,
    IR2_OPND  *opnd0, IR2_OPND *opnd1, IR2_OPND *opnd2, IR2_OPND *opnd3)
{
    IR2_INST *pir2 = ir2_allocate();
    latxs_ir2_build4(pir2, opcode, opnd0, opnd1, opnd2, opnd3);
    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));
    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_ir2_opnd2i(IR2_OPCODE opcode,
        IR2_OPND  *opnd0, IR2_OPND *opnd1, int32_t imm)
{
    IR2_INST *pir2 = ir2_allocate();

    IR2_OPND imm_opnd = latxs_ir2_opnd_new(IR2_OPND_IMMH, imm);
    latxs_ir2_build3(pir2, opcode, opnd0, opnd1, &imm_opnd);

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));

    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_ir2_opnd2(IR2_OPCODE opcode,
        IR2_OPND  *opnd0, IR2_OPND *opnd1)
{
    IR2_INST *pir2 = ir2_allocate();
    latxs_ir2_build2(pir2, opcode, opnd0, opnd1);

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));

    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_ir2_opnd1i(IR2_OPCODE opcode,
        IR2_OPND  *opnd0, int32 imm)
{
    IR2_INST *pir2 = ir2_allocate();
    IR2_OPND imm_opnd = latxs_ir2_opnd_new(IR2_OPND_IMMH, imm);
    latxs_ir2_build2(pir2, opcode, opnd0, &imm_opnd);

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));

    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_ir2_opnd1(IR2_OPCODE opcode,
        IR2_OPND  *opnd0)
{
    IR2_INST *pir2 = ir2_allocate();
    latxs_ir2_build1(pir2, opcode, opnd0);

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));

    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_label(IR2_OPND *label)
{
    return latxs_append_ir2_opnd1(LISA_LABEL, label);
}
IR2_INST *latxs_append_label_with_name(IR2_OPND *label, const char *name)
{
    IR2_INST *pir2 = ir2_allocate();
    latxs_ir2_build1(pir2, LISA_LABEL, label);
    pir2->_b_label_name = name;

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(LISA_LABEL));

    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_ir2_opndi(IR2_OPCODE opcode,
        int32_t imm)
{
    IR2_INST *pir2 = ir2_allocate();
    IR2_OPND imm_opnd = latxs_ir2_opnd_new(IR2_OPND_IMMH, imm);
    latxs_ir2_build1(pir2, opcode, &imm_opnd);

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));

    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_ir2_opnda(IR2_OPCODE opcode,
        ADDR addr)
{
    IR2_INST *pir2 = ir2_allocate();
    IR2_OPND imm_opnd = latxs_ir2_opnd_new(IR2_OPND_IMMH, addr);

    switch (opcode) {
    case LISA_B:
    case LISA_BL:
        lsassert(int32_in_int26(addr));
        latxs_ir2_build1(pir2, opcode, &imm_opnd);
        break;
    case LISA_X86_INST:
#ifdef CONFIG_LATX_TU
    case LISA_TU_SPLIT:
#endif
    case LISA_DUP:
        latxs_ir2_build1(pir2, opcode, &imm_opnd);
        break;
    default:
        lsassertm(0, "append opnda unsupport LISA instruciton (%d)%s\n",
                     opcode, latxs_ir2_name(opcode));
        break;
    }

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));

    ir2_append(pir2);
    return pir2;
}

IR2_INST *latxs_append_ir2_opnd0(
        IR2_OPCODE opcode)
{
    IR2_INST *pir2 = ir2_allocate();
    latxs_ir2_build0(pir2, opcode);

    lsassertm(latxs_ir2_op_check(pir2),
            "Maybe you should check the type of operand %s\n",
            latxs_ir2_name(opcode));

    ir2_append(pir2);
    return pir2;
}

int latxs_ir2_opnd_is_reg_temp(IR2_OPND *opnd)
{
    int reg;
    switch (latxs_ir2_opnd_type(opnd)) {
    case IR2_OPND_GPR: {
        reg = latxs_ir2_opnd_reg(opnd);
#ifdef LATXS_ITMP_CONTINUS
        return LATXS_ITMP_MIN <= reg && reg <= LATXS_ITMP_MAX;
#else
        int i = 0;
        for (; i < latxs_itemp_status_num; i++) {
            if (reg == latxs_itemp_status_default[i].physical_id) {
                return 1;
            }
        }
        return 0;
#endif
    }
    case IR2_OPND_FPR: {
        reg = latxs_ir2_opnd_reg(opnd);
#ifdef LATXS_FTMP_CONTINUS
        return LATXS_FTMP_MIN <= reg && reg <= LATXS_FTMP_MAX;
#else
        int i = 0;
        for (; i < ftemp_status_num; i++) {
            if (reg == ftemp_status_default[i].physical_id) {
                return 1;
            }
        }
        return 0;
#endif
    }
    default: {
        lsassert(0);
        return -1;
    }
    }
}

int latxs_ir2_itemp_index(IR2_OPND *opnd)
{
    if (latxs_ir2_opnd_type(opnd) != IR2_OPND_GPR) {
        return -1;
    }
    const TEMP_REG_STATUS *ireg_map = latxs_itemp_status_default;
    int physical_reg_num = latxs_ir2_opnd_reg(opnd);
    int i = 0;
    for (i = 0; i < latxs_itemp_status_num; ++i) {
        if (ireg_map[i].physical_id == physical_reg_num) {
            return i;
        }
    }
    return -1;
}

IR2_INST *latxs_append_ir2_opnd2_(IR2_OPCODE opcode,
        IR2_OPND  *opnd0, IR2_OPND *opnd1)
{
    IR2_INST *pir2 = NULL;

    switch (opcode) {
    case lisa_mov:
        pir2 = latxs_append_ir2_opnd3(LISA_OR,
                opnd0, opnd1, &latxs_zero_ir2_opnd);
        break;
    case lisa_mov32z:
        pir2 = latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D,
                opnd0, opnd1, 31, 0);
        break;
    case lisa_mov24z:
        pir2 = latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D,
                opnd0, opnd1, 23, 0);
        break;
    case lisa_mov16z:
        pir2 = latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D,
                opnd0, opnd1, 15, 0);
        break;
    case lisa_mov8z:
        pir2 = latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D,
                opnd0, opnd1,  7, 0);
        break;
    case lisa_mov32s:
        pir2 = latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W,
                opnd0, opnd1, 31, 0);
        break;
    case lisa_mov16s:
        pir2 = latxs_append_ir2_opnd2(LISA_EXT_W_H, opnd0, opnd1);
        break;
    case lisa_mov8s:
        pir2 = latxs_append_ir2_opnd2(LISA_EXT_W_B, opnd0, opnd1);
        break;
    case lisa_not:
        pir2 = latxs_append_ir2_opnd3(LISA_NOR,
                opnd0, opnd1, &latxs_zero_ir2_opnd);
        break;
    default:
        lsassertm(0, "append opnd2_ only for fake LISA.\n");
        break;
    }

    return pir2;
}

IR2_INST *latxs_append_ir2_opnd1_(IR2_OPCODE opcode,
        IR2_OPND  *opnd0)
{
    IR2_INST *pir2 = NULL;

    switch (opcode) {
    case lisa_call:
        pir2 = latxs_append_ir2_opnd2i(LISA_JIRL,
                &latxs_ra_ir2_opnd, opnd0, 0);
        break;
    case lisa_jr:
        pir2 = latxs_append_ir2_opnd2i(LISA_JIRL,
                &latxs_zero_ir2_opnd, opnd0, 0);
        break;
    default:
        lsassertm(0, "append opnd1_ only for fake LISA.\n");
        break;
    }

    return pir2;
}

IR2_INST *latxs_append_ir2_opnd0_(IR2_OPCODE opcode)
{
    IR2_INST *pir2 = NULL;

    switch (opcode) {
    case lisa_return:
        pir2 = latxs_append_ir2_opnd2i(LISA_JIRL,
                &latxs_zero_ir2_opnd,
                &latxs_ra_ir2_opnd, 0);
        break;
    case lisa_nop:
        pir2 = latxs_append_ir2_opnd2i(LISA_ANDI,
                &latxs_zero_ir2_opnd,
                &latxs_zero_ir2_opnd, 0);
        break;
    default:
        lsassertm(0, "append opnd0_ only for fake LISA.\n");
        break;
    }

    return pir2;
}

IR2_OPND *latxs_ir2_branch_get_label(IR2_INST *pir2)
{
    int index = latxs_ir2_branch_label_index(pir2);

    if (index >= 0) {
        IR2_OPND *label = &pir2->_opnd[index];
        if (latxs_ir2_opnd_is_label(label)) {
            return label;
        }
    } else {
        lsassertm(0, "unsupported branch label.\n");
    }

    return NULL;
}

IR2_OPND *latxs_ir2_interrupt_get_label(TranslationBlock *tb, int succ_id)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
#ifdef CONFIG_LATX_TU
    if (td->is_tu_mode) {
        td->label_intexit[succ_id] = latxs_ir2_opnd_new(
            IR2_OPND_LABEL, tb->s_data->label_intexit[succ_id]);
        return &td->label_intexit[succ_id];
    }
#endif
    return (IR2_OPND *)&(td->label_intexit[succ_id]);
}

void latxs_ir2_interrupt_set_label(TranslationBlock *tb, int succ_id)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
#ifdef CONFIG_LATX_TU
    if (td->is_tu_mode) {
        td->label_intexit[succ_id] = latxs_ir2_opnd_new(
            IR2_OPND_LABEL, tb->s_data->label_intexit[succ_id]);
        la_label_ptr(&td->label_intexit[succ_id]);
        return;
    }
#endif
    la_label_ptr((IR2_OPND *)&(td->label_intexit[succ_id]));
}

#if defined(CONFIG_LATX_IR2_COMMENT)

void latxs_ir2_add_comment(IR2_INST *pir2, const char *n)
{
    pir2->comment = n;
}

void latxs_ir2_add_comment_on_curr(const char *n)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    IR2_INST *list = td->ir2_array;
    IR2_INST *p = list + td->ir2_cur_nr - 1;
    p->comment = n;
}

#endif

#ifdef CONFIG_LATX_PROFILER

static IR2_INST *la_profile_begin(void)
{
    IR2_OPND label = latxs_ir2_opnd_new_label();
    la_label(label);

    IR2_INST *pir2 = ir2_allocate();
    ir2_set_opcode(pir2, LISA_PROFILE);
    pir2->op_count = 1;
    pir2->_opnd[0] = latxs_ir2_opnd_new(IR2_OPND_PSEUDO, PROFILE_BEGIN);
    pir2->_opnd[1] = label;
    ir2_append(pir2);
    return pir2;
}

static IR2_INST *la_profile_end(void)
{
    IR2_OPND label = latxs_ir2_opnd_new_label();
    la_label(label);

    IR2_INST *pir2 = ir2_allocate();
    ir2_set_opcode(pir2, LISA_PROFILE);
    pir2->op_count = 1;
    pir2->_opnd[0] = latxs_ir2_opnd_new(IR2_OPND_PSEUDO, PROFILE_END);
    pir2->_opnd[1] = label;
    ir2_append(pir2);
    return pir2;
}

void latxs_profile_address(IR2_OPND *dest, TranslationBlock *tb, void *area)
{
#ifdef CONFIG_SOFTMMU
    /* A saved profiler AOT must refer to the newly allocated TB, never the
     * address from the process which produced its ordinary template. */
    ptrdiff_t offset = (uintptr_t)area - (uintptr_t)tb;
    lsassert((uintptr_t)area >= (uintptr_t)&tb->profile &&
             (uintptr_t)area + sizeof(uint64_t) <=
                 (uintptr_t)&tb->profile + sizeof(tb->profile));
    lsassert(offset >= 0 && offset <= 2047);
    latxs_load_addr_to_ir2(dest, (ADDR)tb, LOAD_TB_FULL_ADDR);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, dest, dest, offset);
#else
    latxs_load_imm64_to_ir2(dest, (uintptr_t)area);
#endif
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(dest);
}


void per_tb_count(void *area, int inc)
{
    la_profile_begin();
    /* the function can count TB run times, although all of tb have linked */

    IR2_OPND temp0 = latxs_ra_alloc_itemp();
    IR2_OPND temp1 = latxs_ra_alloc_itemp();

    ptrdiff_t addr = (ptrdiff_t)area;
    lsassert((addr & 0b111) == 0);
    latxs_profile_address(&temp0, lsenv->tr_data->curr_tb, area);
    latxs_load_imm64_to_ir2(&temp1, 1);
    latxs_append_ir2_opnd3(LISA_AMADD_D, &latxs_zero_ir2_opnd, &temp0, &temp1);
    latxs_ra_free_temp(&temp0);
    latxs_ra_free_temp(&temp1);
    la_profile_end();
}
#endif
