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
#include "latx-sys-tr-sse42.h"
#include "sys-aot.h"

#define XMM_REG(o) ir1_opnd_base_reg_num(o)
#define XMM_LOAD128(o) latxs_load_freg128_from_ir1(o)
#define XMM_LOADFP1(o) latxs_load_freg_from_ir1_1(o, true)

#define XMM_EXCP(pir1) do {                             \
    if (latxs_tr_gen_sse_common_excp_check(pir1)) {     \
        return true;                                    \
    }                                                   \
} while (0)

void latxs_sys_sse42_register_ir1(void)
{
    latxs_register_ir1(X86_INS_PCMPGTQ);
    latxs_register_ir1(X86_INS_PCMPISTRI);
    latxs_register_ir1(X86_INS_PCMPESTRI);
    latxs_register_ir1(X86_INS_PCMPISTRM);
    latxs_register_ir1(X86_INS_PCMPESTRM);
    latxs_register_ir1(X86_INS_CRC32);
}

bool latxs_translate_pcmpgtq(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 0)));       
    IR2_OPND dest = XMM_LOAD128(ir1_get_opnd(pir1, 0));     
    IR2_OPND src = XMM_LOAD128(ir1_get_opnd(pir1, 1));      
    latxs_append_ir2_opnd3(LISA_VSLT_D, &dest, &src, &dest);

    return true;
}

bool latxs_translate_pcmpistri(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    int imm = ir1_opnd_uimm(opnd2);
    int d = XMM_REG(opnd0);

    if (ir1_opnd_is_xmm(opnd1)) {
        int s = XMM_REG(opnd1);
        latxs_tr_gen_call_to_helper_pcmpxstrx((ADDR)helper_pcmpistri_xmm, d, s, imm,
                HELPER_PCMPISTRI_XMM);
    } else {
        IR2_OPND temp = latxs_ra_alloc_ftemp();
        IR2_OPND src = latxs_ra_alloc_xmm((d + 1) % 8);
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &temp, &src, &src);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &temp, &src, &src);
        }
        latxs_load_freg128_from_ir1_mem(&src, opnd1);
        latxs_tr_gen_call_to_helper_pcmpxstrx((ADDR)helper_pcmpistri_xmm, d, (d + 1) % 8, imm,
                HELPER_PCMPISTRI_XMM);
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &src, &temp, &temp);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &src, &temp, &temp);
        }
    }
    return true;
}

bool latxs_translate_pcmpestri(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    int imm = ir1_opnd_uimm(opnd2);
    int d = XMM_REG(opnd0);
    /* Presence of REX.W is indicated by bit 8*/
    imm |= ir1_rex_w(pir1) << 8;

    if (ir1_opnd_is_xmm(opnd1)) {
        int s = XMM_REG(opnd1);
        latxs_tr_gen_call_to_helper_pcmpxstrx((ADDR)helper_pcmpestri_xmm, d, s, imm,
                HELPER_PCMPESTRI_XMM);
    } else {
        IR2_OPND temp = latxs_ra_alloc_ftemp();
        IR2_OPND src = latxs_ra_alloc_xmm((d + 1) % 8);
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &temp, &src, &src);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &temp, &src, &src);
        }
        latxs_load_freg128_from_ir1_mem(&src, opnd1);
        latxs_tr_gen_call_to_helper_pcmpxstrx((ADDR)helper_pcmpestri_xmm, d, (d + 1) % 8, imm,
                HELPER_PCMPESTRI_XMM);
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &src, &temp, &temp);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &src, &temp, &temp);
        }
    }
    return true;
}

bool latxs_translate_pcmpistrm(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    int imm = ir1_opnd_uimm(opnd2);
    int d = XMM_REG(opnd0);

    if (ir1_opnd_is_xmm(opnd1)) {
        int s = XMM_REG(opnd1);
        latxs_tr_gen_call_to_helper_pcmpxstrx((ADDR)helper_pcmpistrm_xmm, d, s, imm,
                HELPER_PCMPISTRM_XMM);
    } else {
        IR2_OPND temp = latxs_ra_alloc_ftemp();
        IR2_OPND src = latxs_ra_alloc_xmm((d + 1) % 7 + 1);
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &temp, &src, &src);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &temp, &src, &src);
        }
        latxs_load_freg128_from_ir1_mem(&src, opnd1);
        latxs_tr_gen_call_to_helper_pcmpxstrx((ADDR)helper_pcmpistrm_xmm, d, (d + 1) % 7 + 1, imm,
                HELPER_PCMPISTRM_XMM);
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &src, &temp, &temp);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &src, &temp, &temp);
        }
    }
    return true;
}

bool latxs_translate_pcmpestrm(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    int imm = ir1_opnd_uimm(opnd2);
    int d = XMM_REG(opnd0);
    /* Presence of REX.W is indicated by bit 8*/
    imm |= ir1_rex_w(pir1) << 8;

    if (ir1_opnd_is_xmm(opnd1)) {
        int s = XMM_REG(opnd1);
        latxs_tr_gen_call_to_helper_pcmpxstrx((ADDR)helper_pcmpestrm_xmm, d, s, imm,
                HELPER_PCMPESTRM_XMM);
    } else {
        IR2_OPND temp = latxs_ra_alloc_ftemp();
        IR2_OPND src = latxs_ra_alloc_xmm((d + 1) % 7 + 1);
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &temp, &src, &src);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &temp, &src, &src);
        }
        latxs_load_freg128_from_ir1_mem(&src, opnd1);
        latxs_tr_gen_call_to_helper_pcmpxstrx((ADDR)helper_pcmpestrm_xmm, d, (d + 1) % 7 + 1, imm,
                HELPER_PCMPESTRM_XMM);
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &src, &temp, &temp);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &src, &temp, &temp);
        }
    }
    return true;
}

bool latxs_translate_crc32(IR1_INST *pir1)
{
    if (latxs_tr_gen_excp_check(pir1)) return true; // SSE42

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src0 = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
    IR2_OPND src1;
    if (ir1_opnd_is_gpr(opnd1)) {
        src1 = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd1));
    } else {
        src1 = la_new_itmp();
        latxs_load_ir1_to_ir2(&src1, opnd1, EXMode_N);
    }
    IR2_OPND dest = la_new_itmp();

//    CRC32 r32, r/m8
//    CRC32 r32, r/m8*
//    CRC32 r32, r/m16
//    CRC32 r32, r/m32
//    CRC32 r64, r/m8
//    CRC32 r64, r/m64
    int opnd0_size = ir1_opnd_size(opnd0);
    int opnd1_size = ir1_opnd_size(opnd1);

    if (opnd1_size == 8) {
        la_out3(LISA_CRCC_W_B_W, &dest, &src1, &src0);
    } else if(opnd1_size == 16) {
        la_out3(LISA_CRCC_W_H_W, &dest, &src1, &src0);
    } else if(opnd1_size == 32) {
        la_out3(LISA_CRCC_W_W_W, &dest, &src1, &src0);
    } else if(opnd0_size == 64) {
        la_out3(LISA_CRCC_W_D_W, &dest, &src1, &src0);
    } else {
        fprintf(stderr, "%s:%d CRC32 unknown opnd size\n", __func__, __LINE__);
    }

//    latxs_store_ir2_to_ir1(&dest, opnd0);
    la_out2_(lisa_mov32z, &src0, &dest);

    return true;
}
