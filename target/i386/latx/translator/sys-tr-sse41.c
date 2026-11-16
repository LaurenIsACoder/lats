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
#include "latx-shbr.h"
#include "translate.h"
#include "sys-excp.h"
#include "latx-sys-tr-sse41.h"

void latxs_sys_sse41_register_ir1(void)
{
    latxs_register_ir1(X86_INS_PTEST);
    latxs_register_ir1(X86_INS_PEXTRQ);
    latxs_register_ir1(X86_INS_PEXTRD);
    latxs_register_ir1(X86_INS_PEXTRB);
    latxs_register_ir1(X86_INS_PINSRB);
    latxs_register_ir1(X86_INS_PINSRD);
    latxs_register_ir1(X86_INS_PINSRQ);
    latxs_register_ir1(X86_INS_PMOVZXBQ);
    latxs_register_ir1(X86_INS_PMOVZXDQ);
    latxs_register_ir1(X86_INS_PMOVZXBW);
    latxs_register_ir1(X86_INS_PMOVZXBD);
    latxs_register_ir1(X86_INS_PMOVZXWD);
    latxs_register_ir1(X86_INS_PMOVZXWQ);
    latxs_register_ir1(X86_INS_PMOVSXBW);
    latxs_register_ir1(X86_INS_PMOVSXDQ);
    latxs_register_ir1(X86_INS_PMOVSXBD);
    latxs_register_ir1(X86_INS_PMOVSXWD);
    latxs_register_ir1(X86_INS_PMOVSXBQ);
    latxs_register_ir1(X86_INS_PMOVSXWQ);
    latxs_register_ir1(X86_INS_BLENDVPD);
    latxs_register_ir1(X86_INS_BLENDVPS);
    latxs_register_ir1(X86_INS_ROUNDSS);
    latxs_register_ir1(X86_INS_ROUNDPS);
    latxs_register_ir1(X86_INS_ROUNDSD);
    latxs_register_ir1(X86_INS_ROUNDPD);
    latxs_register_ir1(X86_INS_PCMPEQQ);
    latxs_register_ir1(X86_INS_PBLENDW);
    latxs_register_ir1(X86_INS_PBLENDVB);
    latxs_register_ir1(X86_INS_INSERTPS);
    latxs_register_ir1(X86_INS_EXTRACTPS);
    latxs_register_ir1(X86_INS_PMINUD);
    latxs_register_ir1(X86_INS_PMINSD);
    latxs_register_ir1(X86_INS_PMINUW);
    latxs_register_ir1(X86_INS_PMINSB);
    latxs_register_ir1(X86_INS_PMULLD);
    latxs_register_ir1(X86_INS_PMULDQ);
    latxs_register_ir1(X86_INS_PMAXSD);
    latxs_register_ir1(X86_INS_PMAXUD);
    latxs_register_ir1(X86_INS_PMAXSB);
    latxs_register_ir1(X86_INS_PMAXUW);
    latxs_register_ir1(X86_INS_PACKUSDW);
    latxs_register_ir1(X86_INS_DPPD);
    latxs_register_ir1(X86_INS_DPPS);
    latxs_register_ir1(X86_INS_MOVNTDQA);
    latxs_register_ir1(X86_INS_BLENDPD);
    latxs_register_ir1(X86_INS_BLENDPS);
    latxs_register_ir1(X86_INS_MPSADBW);
    latxs_register_ir1(X86_INS_PHMINPOSUW);
}

#define XMM_REG(o) ir1_opnd_base_reg_num(o)
#define XMM_LOAD128(o) latxs_load_freg128_from_ir1(o)
#define XMM_LOADFP1(o) latxs_load_freg_from_ir1_1(o, true)

#define SHBR_ACTIVE_32(ir1) \
    (option_shbr && lsenv->tr_data->is_tu_mode && LATXS_SHBR_ON_32(ir1))
#define SHBR_ACTIVE_64(ir1) \
    (option_shbr && lsenv->tr_data->is_tu_mode && LATXS_SHBR_ON_64(ir1))

#define XMM_EXCP(pir1) do {                            \
    if (latxs_tr_gen_sse_common_excp_check(pir1)) {    \
        return true;                                   \
    }                                                  \
} while (0)

bool latxs_translate_ptest(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);
    IR2_OPND temp = latxs_ra_alloc_ftemp();
    IR2_OPND label_1 = latxs_ir2_opnd_new_label();
    IR2_OPND label_2 = latxs_ir2_opnd_new_label();
    IR2_OPND n4095_opnd = latxs_ra_alloc_itemp();             
    latxs_append_ir2_opnd2i(LISA_ORI, &n4095_opnd, &latxs_zero_ir2_opnd, 0xfff);

    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &latxs_zero_ir2_opnd, 0x3f);
    latxs_append_ir2_opnd3(LISA_VAND_V, &temp, &src, &dest);
    latxs_append_ir2_opnd2(LISA_VSETEQZ_V, &latxs_fcc0_ir2_opnd, &temp);
    latxs_append_ir2_opnd2(LISA_BCEQZ, &latxs_fcc0_ir2_opnd, &label_1);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &n4095_opnd, ZF_USEDEF_BIT);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_1);
    latxs_append_ir2_opnd3(LISA_VANDN_V, &temp, &dest, &src);
    latxs_append_ir2_opnd2(LISA_VSETEQZ_V, &latxs_fcc0_ir2_opnd, &temp);
    latxs_append_ir2_opnd2(LISA_BCEQZ, &latxs_fcc0_ir2_opnd, &label_2);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &n4095_opnd, CF_USEDEF_BIT);
    latxs_append_ir2_opnd1(LISA_LABEL, &label_2);

    latxs_ra_free_temp(&n4095_opnd);
    latxs_ra_free_temp(&temp);
    return true;
}

bool latxs_translate_pextrq(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    IR2_OPND dest;                              
    IR2_OPND src = XMM_LOAD128(opnd1);
    uint8_t imm = ir1_opnd_uimm(opnd2) & 0x1;

    if (ir1_opnd_is_gpr(opnd0)) {
        dest = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2i(LISA_VPICKVE2GR_D, &dest, &src, imm);
    } else {
        dest = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_VPICKVE2GR_D, &dest, &src, imm);
        latxs_store_ir2_to_ir1(&dest, opnd0);
        latxs_ra_free_temp(&dest);
    }
    return true;
}

bool latxs_translate_pextrd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    IR2_OPND dest;                              
    IR2_OPND src = XMM_LOAD128(opnd1);
    uint8_t imm = ir1_opnd_uimm(opnd2) & 0x3;

    if (ir1_opnd_is_gpr(opnd0)) {
        dest = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2i(LISA_VPICKVE2GR_WU, &dest, &src, imm);
    } else {
        dest = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_VPICKVE2GR_WU, &dest, &src, imm);
        latxs_store_ir2_to_ir1(&dest, opnd0);
        latxs_ra_free_temp(&dest);
    }
    return true;
}

bool latxs_translate_pextrb(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    IR2_OPND dest;                              
    IR2_OPND src = XMM_LOAD128(opnd1);
    uint8_t imm = ir1_opnd_uimm(opnd2) & 0xF;

    if (ir1_opnd_is_gpr(opnd0)) {
        dest = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2i(LISA_VPICKVE2GR_BU, &dest, &src, imm);
    } else {
        dest = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_VPICKVE2GR_BU, &dest, &src, imm);
        latxs_store_ir2_to_ir1(&dest, opnd0);
        latxs_ra_free_temp(&dest);
    }
    return true;
}

bool latxs_translate_pinsrb(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    uint8_t imm = ir1_opnd_uimm(opnd2) & 0xF;

    latxs_append_ir2_opnd2i(LISA_VINSGR2VR_B, &dest, &src, imm);
    latxs_ra_free_temp(&src);
    return true;
}

bool latxs_translate_pinsrd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    uint8_t imm = ir1_opnd_uimm(opnd2) & 0x3;

    latxs_append_ir2_opnd2i(LISA_VINSGR2VR_W, &dest, &src, imm);
    latxs_ra_free_temp(&src);
    return true;
}

bool latxs_translate_pinsrq(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = latxs_load_ireg_from_ir1(opnd1, EXMode_N);
    uint8_t imm = ir1_opnd_uimm(opnd2) & 0x1;

    latxs_append_ir2_opnd2i(LISA_VINSGR2VR_D, &dest, &src, imm);
    latxs_ra_free_temp(&src);
    return true;
}

bool latxs_translate_pmovzxbq(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_DU_BU, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_HU_BU, &dest, &src, 0);
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_WU_HU, &dest, &dest, 0);
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_DU_WU, &dest, &dest, 0);
    }
    return true; 
}

bool latxs_translate_pmovsxbq(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_D_B, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_H_B, &dest, &src, 0);
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_W_H, &dest, &dest, 0);
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_D_W, &dest, &dest, 0);
    }
    return true; 
}

bool latxs_translate_pmovzxwd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_WU_HU, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_WU_HU, &dest, &src, 0);
    }
    return true; 
}

bool latxs_translate_pmovsxwd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_W_H, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_W_H, &dest, &src, 0);
    }
    return true; 
}

bool latxs_translate_pmovzxwq(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_DU_HU, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_WU_HU, &dest, &src, 0);
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_DU_WU, &dest, &dest, 0);
    }
    return true; 
}

bool latxs_translate_pmovsxwq(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_D_H, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_W_H, &dest, &src, 0);
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_D_W, &dest, &dest, 0);
    }
    return true; 
}

bool latxs_translate_pmovsxdq(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_D_W, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_D_W, &dest, &src, 0);
    }
    return true; 
}

bool latxs_translate_pmovsxbd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_W_B, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_H_B, &dest, &src, 0);
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_W_H, &dest, &dest, 0);
    }
    return true; 
}

bool latxs_translate_pmovzxdq(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_DU_WU, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_DU_WU, &dest, &src, 0);
    }
    return true; 
}

bool latxs_translate_pmovzxbw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_HU_BU, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_HU_BU, &dest, &src, 0);
    }
    return true; 
}

bool latxs_translate_pmovsxbw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_H_B, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_H_B, &dest, &src, 0);
    }
    return true; 
}

bool latxs_translate_pmovzxbd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_VEXT2XV_WU_BU, &dest, &src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_HU_BU, &dest, &src, 0);
        latxs_append_ir2_opnd2i(LISA_VSLLWIL_WU_HU, &dest, &dest, 0);
    }
    return true; 
}

bool latxs_translate_blendvpd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);
    IR2_OPND mask = latxs_ra_alloc_xmm(0);
    IR2_OPND temp = latxs_ra_alloc_ftemp();

    latxs_append_ir2_opnd2i(LISA_VSLTI_D, &temp, &mask, 0);
    latxs_append_ir2_opnd4(LISA_VBITSEL_V, &dest, &dest, &src, &temp);

    return true;
}

bool latxs_translate_blendvps(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);
    IR2_OPND mask = latxs_ra_alloc_xmm(0);
    IR2_OPND temp = latxs_ra_alloc_ftemp();

    latxs_append_ir2_opnd2i(LISA_VSLTI_W, &temp, &mask, 0);
    latxs_append_ir2_opnd4(LISA_VBITSEL_V, &dest, &dest, &src, &temp);

    return true;
}

bool latxs_translate_roundss(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    lsassert(ir1_opnd_is_xmm(opnd0));
    uint8_t imm = ir1_opnd_uimm(opnd2);

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND temp_src = XMM_LOAD128(opnd1);
    IR2_OPND temp1;

    temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND fcsr = latxs_ra_alloc_itemp();
    IR2_OPND src = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_itemp();
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    
    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_XVREPLVE0_W, &src, &temp_src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VREPLVEI_W, &src, &temp_src, 0);
    }
    if (imm & 0x8) {
        latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &fcsr, &latxs_fcsr2_ir2_opnd);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp2, &fcsr, 16, 16);
    }
    if (imm & 0x4) {
        latxs_append_ir2_opnd2(LISA_VFRINT_S, &temp1, &src);
    } else if ((imm & 0x3) == 0x0) {
        latxs_append_ir2_opnd2(LISA_VFRINTRNE_S, &temp1, &src);
    } else if ((imm & 0x3) == 0x1) {
        latxs_append_ir2_opnd2(LISA_VFRINTRM_S, &temp1, &src);
    } else if ((imm & 0x3) == 0x2) {
        latxs_append_ir2_opnd2(LISA_VFRINTRP_S, &temp1, &src);
    } else if ((imm & 0x3) == 0x3) {
        latxs_append_ir2_opnd2(LISA_VFRINTRZ_S, &temp1, &src);
    }
    latxs_tr_gen_simd_fpe_check_t(pir1, NULL);
    if(imm & 0x8) {
        latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &fcsr, &latxs_fcsr2_ir2_opnd);
        /* PE */
        IR2_OPND temp3 = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp3, &fcsr, 16, 16);
        latxs_append_ir2_opnd3(LISA_XOR, &temp2, &temp2, &temp3);
        latxs_append_ir2_opnd3(LISA_BEQ, &temp2, &latxs_zero_ir2_opnd, &label_exit);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &fcsr, &latxs_zero_ir2_opnd, FCSR_OFF_FLAGS_I, FCSR_OFF_FLAGS_I);
        latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr2_ir2_opnd, &fcsr);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    }
    if (SHBR_ACTIVE_32(pir1)) {
        latxs_append_ir2_opnd2(LISA_FMOV_S, &dest, &temp1);
    } else {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &temp1,
                                VEXTRINS_IMM_4_0(0, 0));
    }
    return true;
}

bool latxs_translate_roundsd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    uint8_t imm = ir1_opnd_uimm(opnd2);
    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND temp_src = XMM_LOAD128(opnd1);

    IR2_OPND temp1;

    temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND fcsr = latxs_ra_alloc_itemp();
    IR2_OPND src = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_itemp();
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();

    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_XVREPLVE0_D, &src, &temp_src);
    } else {
        latxs_append_ir2_opnd2i(LISA_VREPLVEI_D, &src, &temp_src, 0);
    }
    if (imm & 0x8) {
        latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &fcsr, &latxs_fcsr2_ir2_opnd);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp2, &fcsr, 16, 16);
    }
    if (imm & 0x4) {
        latxs_append_ir2_opnd2(LISA_VFRINT_D, &temp1, &src);
    } else if ((imm & 0x3) == 0x0) {
        latxs_append_ir2_opnd2(LISA_VFRINTRNE_D, &temp1, &src);
    } else if ((imm & 0x3) == 0x1) {
        latxs_append_ir2_opnd2(LISA_VFRINTRM_D, &temp1, &src);
    } else if ((imm & 0x3) == 0x2) {
        latxs_append_ir2_opnd2(LISA_VFRINTRP_D, &temp1, &src);
    } else if ((imm & 0x3) == 0x3) {
        latxs_append_ir2_opnd2(LISA_VFRINTRZ_D, &temp1, &src);
    }
    latxs_tr_gen_simd_fpe_check_t(pir1, NULL);
    if(imm & 0x8) {
        latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &fcsr, &latxs_fcsr2_ir2_opnd);
        // PE
        IR2_OPND temp3 = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp3, &fcsr, 16, 16);
        latxs_append_ir2_opnd3(LISA_XOR, &temp2, &temp2, &temp3);
        latxs_append_ir2_opnd3(LISA_BEQ, &temp2, &latxs_zero_ir2_opnd, &label_exit);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &fcsr, &latxs_zero_ir2_opnd, FCSR_OFF_FLAGS_I, FCSR_OFF_FLAGS_I);
        latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr2_ir2_opnd, &fcsr);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    }
    if (SHBR_ACTIVE_64(pir1)) {
        latxs_append_ir2_opnd2(LISA_FMOV_D, &dest, &temp1);
    } else {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &dest, &temp1,
                                VEXTRINS_IMM_4_0(0, 0));
    }
    return true;
}

bool latxs_translate_roundpd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    lsassert(ir1_opnd_is_xmm(opnd0));
    uint8_t imm = ir1_opnd_uimm(opnd2);

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    IR2_OPND fcsr = latxs_ra_alloc_itemp();
    IR2_OPND temp2 = latxs_ra_alloc_itemp();
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();

    if (imm & 0x8) {
        latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &fcsr, &latxs_fcsr2_ir2_opnd);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp2, &fcsr, 16, 16);
    }

    IR2_OPND dest_t;
    if (option_sigfpe) {
        dest_t = latxs_ra_alloc_ftemp();
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &dest_t, &dest, &dest);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &dest_t, &dest, &dest);
        }
    }

    if (imm & 0x4) {
        latxs_append_ir2_opnd2(LISA_VFRINT_D, &dest, &src);
    } else if ((imm & 0x3) == 0x0) {
        latxs_append_ir2_opnd2(LISA_VFRINTRNE_D, &dest, &src);
    } else if ((imm & 0x3) == 0x1) {
        latxs_append_ir2_opnd2(LISA_VFRINTRM_D, &dest, &src);
    } else if ((imm & 0x3) == 0x2) {
        latxs_append_ir2_opnd2(LISA_VFRINTRP_D, &dest, &src);
    } else if ((imm & 0x3) == 0x3) {
        latxs_append_ir2_opnd2(LISA_VFRINTRZ_D, &dest, &src);
    }
    latxs_tr_gen_simd_fpe_check_t(pir1, &dest_t);
    if(imm & 0x8) {
        latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &fcsr, &latxs_fcsr2_ir2_opnd);
        // PE
        IR2_OPND temp3 = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp3, &fcsr, 16, 16);
        latxs_append_ir2_opnd3(LISA_XOR, &temp2, &temp2, &temp3);
        latxs_append_ir2_opnd3(LISA_BEQ, &temp2, &latxs_zero_ir2_opnd, &label_exit);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &fcsr, &latxs_zero_ir2_opnd, FCSR_OFF_FLAGS_I, FCSR_OFF_FLAGS_I);
        latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr2_ir2_opnd, &fcsr);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    }
    return true;
}

bool latxs_translate_roundps(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    lsassert(ir1_opnd_is_xmm(opnd0));
    uint8_t imm = ir1_opnd_uimm(opnd2);

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    IR2_OPND fcsr = latxs_ra_alloc_itemp();
    IR2_OPND temp2 = latxs_ra_alloc_itemp();
    IR2_OPND label_exit = latxs_ir2_opnd_new_label();

    if (imm & 0x8) {
        latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &fcsr, &latxs_fcsr2_ir2_opnd);
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp2, &fcsr, 16, 16);
    }

    IR2_OPND dest_t;
    if (option_sigfpe) {
        dest_t = latxs_ra_alloc_ftemp();
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &dest_t, &dest, &dest);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &dest_t, &dest, &dest);
        }
    }

    if (imm & 0x4) {
        latxs_append_ir2_opnd2(LISA_VFRINT_S, &dest, &src);
    } else if ((imm & 0x3) == 0x0) {
        latxs_append_ir2_opnd2(LISA_VFRINTRNE_S, &dest, &src);
    } else if ((imm & 0x3) == 0x1) {
        latxs_append_ir2_opnd2(LISA_VFRINTRM_S, &dest, &src);
    } else if ((imm & 0x3) == 0x2) {
        latxs_append_ir2_opnd2(LISA_VFRINTRP_S, &dest, &src);
    } else if ((imm & 0x3) == 0x3) {
        latxs_append_ir2_opnd2(LISA_VFRINTRZ_S, &dest, &src);
    }

    latxs_tr_gen_simd_fpe_check_t(pir1, &dest_t);

    if(imm & 0x8) {
        latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &fcsr, &latxs_fcsr2_ir2_opnd);
        // PE
        IR2_OPND temp3 = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp3, &fcsr, 16, 16);
        latxs_append_ir2_opnd3(LISA_XOR, &temp2, &temp2, &temp3);
        latxs_append_ir2_opnd3(LISA_BEQ, &temp2, &latxs_zero_ir2_opnd, &label_exit);
        latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &fcsr, &latxs_zero_ir2_opnd, FCSR_OFF_FLAGS_I, FCSR_OFF_FLAGS_I);
        latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr2_ir2_opnd, &fcsr);

        latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    }
    return true;
}
bool latxs_translate_pcmpeqq(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 0)));
    IR2_OPND dest = XMM_LOAD128(ir1_get_opnd(pir1, 0));
    IR2_OPND src = XMM_LOAD128(ir1_get_opnd(pir1, 1));
    latxs_append_ir2_opnd3(LISA_VSEQ_D, &dest, &dest, &src);

    return true;
}

bool latxs_translate_pblendw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);
    uint8_t imm = ir1_opnd_uimm(opnd2);

    if (imm == 0xff) {
        latxs_append_ir2_opnd3(LISA_VAND_V, &dest, &src, &src);
        return true;              
    }

    /* 64 bit fast path */
    if ((imm & 0xf) == 0xf) {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &dest, &src, VEXTRINS_IMM_4_0(0, 0));
        imm &= ~0xf;
    }
    if ((imm & 0xf0) == 0xf0) {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &dest, &src, VEXTRINS_IMM_4_0(1, 1));
        imm &= ~0xf0;
    }

    /* 32 bit fast path */
    if ((imm & 0x3) == 0x3) {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &src, VEXTRINS_IMM_4_0(0, 0));
        imm &= ~0x3;
    }
    if ((imm & 0xc) == 0xc) {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &src, VEXTRINS_IMM_4_0(1, 1));
        imm &= ~0xc;
    }
    if ((imm & 0x30) == 0x30) {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &src, VEXTRINS_IMM_4_0(2, 2));
        imm &= ~0x30;
    }
    if ((imm & 0xc0) == 0xc0) {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &src, VEXTRINS_IMM_4_0(3, 3));
        imm &= ~0xc0;
    }

    /* 16 bit slow path */
    if (imm & 0x1)                                       
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &dest, &src, VEXTRINS_IMM_4_0(0, 0));
    if (imm & 0x2)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &dest, &src, VEXTRINS_IMM_4_0(1, 1));
    if (imm & 0x4)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &dest, &src, VEXTRINS_IMM_4_0(2, 2));
    if (imm & 0x8)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &dest, &src, VEXTRINS_IMM_4_0(3, 3));
    if (imm & 0x10)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &dest, &src, VEXTRINS_IMM_4_0(4, 4));
    if (imm & 0x20)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &dest, &src, VEXTRINS_IMM_4_0(5, 5));
    if (imm & 0x40)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &dest, &src, VEXTRINS_IMM_4_0(6, 6));
    if (imm & 0x80)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_H, &dest, &src, VEXTRINS_IMM_4_0(7, 7));

    return true;
}

bool latxs_translate_insertps(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    lsassert(ir1_opnd_is_xmm(opnd0));
    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    uint8_t imm = ir1_opnd_uimm(opnd2);
    uint8_t count_s = (imm >> 6) & 0x3;
    uint8_t count_d = (imm >> 4) & 0x3;
    uint8_t zmask = imm & 0xf;

    if (ir1_opnd_is_mem(opnd1)) {
        count_s = 0;
    }
    latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &src, VEXTRINS_IMM_4_0(count_d, count_s));

    latxs_append_ir2_opnd3(LISA_VXOR_V, &temp1, &temp1, &temp1);
    if(zmask & 0x1)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &temp1, VEXTRINS_IMM_4_0(0, 0));
    if(zmask & 0x2) 
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &temp1, VEXTRINS_IMM_4_0(1, 0));
    if(zmask & 0x4)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &temp1, VEXTRINS_IMM_4_0(2, 0));
    if(zmask & 0x8)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &temp1, VEXTRINS_IMM_4_0(3, 0));
    return true;
}

bool latxs_translate_extractps(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    IR2_OPND src = XMM_LOAD128(opnd1);
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    lsassert(ir1_opnd_is_gpr(opnd0) || ir1_opnd_is_mem(opnd0));

    uint8_t imm = ir1_opnd_uimm(opnd2);
    uint8_t src_offset = imm & 0x3;    

    latxs_append_ir2_opnd3(LISA_VXOR_V, &temp1, &temp1, &temp1);
    latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &temp1, &src, VEXTRINS_IMM_4_0(0, src_offset));

    IR2_OPND dest_opnd = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2(LISA_MOVFR2GR_S, &dest_opnd, &temp1);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &dest_opnd, &dest_opnd, 31, 0);
    latxs_store_ir2_to_ir1(&dest_opnd, opnd0);
    return true;
}

bool latxs_translate_pminud(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    latxs_append_ir2_opnd3(LISA_VMIN_WU, &dest, &dest, &src);
    return true; 
}

bool latxs_translate_pminsb(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    latxs_append_ir2_opnd3(LISA_VMIN_B, &dest, &dest, &src);
    return true; 
}

bool latxs_translate_pminuw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    latxs_append_ir2_opnd3(LISA_VMIN_HU, &dest, &dest, &src);
    return true; 
}

bool latxs_translate_pmulld(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    latxs_append_ir2_opnd3(LISA_VMUL_W, &dest, &dest, &src);
    return true; 
}

bool latxs_translate_pmuldq(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    latxs_append_ir2_opnd3(LISA_VMULWEV_D_W, &dest, &dest, &src);
    return true; 
}

bool latxs_translate_pmaxsd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    latxs_append_ir2_opnd3(LISA_VMAX_W, &dest, &dest, &src);
    return true; 
}

bool latxs_translate_pmaxud(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    latxs_append_ir2_opnd3(LISA_VMAX_WU, &dest, &dest, &src);
    return true; 
}

bool latxs_translate_pmaxsb(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    latxs_append_ir2_opnd3(LISA_VMAX_B, &dest, &dest, &src);
    return true; 
}

bool latxs_translate_pmaxuw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    latxs_append_ir2_opnd3(LISA_VMAX_HU, &dest, &dest, &src);
    return true; 
}

bool latxs_translate_pminsd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);

    latxs_append_ir2_opnd3(LISA_VMIN_W, &dest, &dest, &src);
    return true; 
}

bool latxs_translate_packusdw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);
    IR2_OPND temp = latxs_ra_alloc_ftemp();

    if(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 1)) && (ir1_opnd_base_reg_num(opnd0) == ir1_opnd_base_reg_num(opnd1))) {
        latxs_append_ir2_opnd2i(LISA_VSLTI_W, &temp, &dest, 0);
        latxs_append_ir2_opnd3(LISA_VANDN_V, &dest, &temp, &dest);
        latxs_append_ir2_opnd2i(LISA_VSSRANI_HU_W, &dest, &dest, 0);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &dest, &dest, VEXTRINS_IMM_4_0(1, 0));
    } else {
        latxs_append_ir2_opnd2i(LISA_VSLTI_W, &temp, &dest, 0);
        latxs_append_ir2_opnd3(LISA_VANDN_V, &dest, &temp, &dest);
        latxs_append_ir2_opnd2i(LISA_VSSRANI_HU_W, &dest, &dest, 0);

        IR2_OPND temp1 = latxs_ra_alloc_ftemp();
        latxs_append_ir2_opnd2i(LISA_VSLTI_W, &temp1, &src, 0);
        latxs_append_ir2_opnd3(LISA_VANDN_V, &temp1, &temp1, &src);
        latxs_append_ir2_opnd2i(LISA_VSSRANI_HU_W, &temp1, &temp1, 0);

        /* src packed to high part */
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &dest, &temp1, VEXTRINS_IMM_4_0(1, 0));
    }
    return true; 
}

bool latxs_translate_dpps(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src1 = XMM_LOAD128(opnd1);
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();
    uint8_t imm = ir1_opnd_uimm(opnd2);

    latxs_append_ir2_opnd3(LISA_VXOR_V, &temp1, &temp1, &temp1);
    latxs_append_ir2_opnd3(LISA_VXOR_V, &temp2, &temp2, &temp2);
    if(imm & 0x10){
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &temp1, &dest, 0x00);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &temp2, &src1, 0x00);
    }
    if(imm & 0x20){
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &temp1, &dest, 0x11);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &temp2, &src1, 0x11);
    }
    if(imm & 0x40){
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &temp1, &dest, 0x22);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &temp2, &src1, 0x22);
    }
    if(imm & 0x80){
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &temp1, &dest, 0x33);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &temp2, &src1, 0x33);
    }
    latxs_append_ir2_opnd3(LISA_VFMUL_S, &temp1, &temp1, &temp2);
    latxs_tr_gen_simd_fpe_check_t(pir1, NULL);

    latxs_append_ir2_opnd3(LISA_VPACKOD_W, &temp2, &temp1, &temp1);
    latxs_append_ir2_opnd3(LISA_VPACKEV_W, &temp1, &temp1, &temp1);
    latxs_append_ir2_opnd3(LISA_VFADD_S, &temp1, &temp1, &temp2);
    latxs_tr_gen_simd_fpe_check_t(pir1, NULL);

    latxs_append_ir2_opnd3(LISA_VPACKOD_D, &temp2, &temp1, &temp1);
    latxs_append_ir2_opnd3(LISA_VPACKEV_D, &temp1, &temp1, &temp1);
    latxs_append_ir2_opnd3(LISA_VFADD_S, &temp1, &temp1, &temp2);
    latxs_tr_gen_simd_fpe_check_t(pir1, NULL);

    IR2_OPND dest_t;
    if (option_sigfpe) {
        dest_t = latxs_ra_alloc_ftemp();
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &dest_t, &dest, &dest);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &dest_t, &dest, &dest);
        }
    }

    latxs_append_ir2_opnd3(LISA_VXOR_V, &dest, &dest, &dest);
    if(imm & 0x1){
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &temp1, 0x00);
    }
    if(imm & 0x2){
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &temp1, 0x11);
    }
    if(imm & 0x4){
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &temp1, 0x22);
    }
    if(imm & 0x8){
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &temp1, 0x33);
    }
    if (option_enable_lasx) {
        latxs_append_ir2_opnd2i(LISA_XVINSGR2VR_D, &dest, &latxs_zero_ir2_opnd, 2);
        latxs_append_ir2_opnd2i(LISA_XVINSGR2VR_D, &dest, &latxs_zero_ir2_opnd, 3);
    }
    latxs_tr_gen_simd_fpe_check_t(pir1, &dest_t);
    return true;
}

bool latxs_translate_dppd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src1 = XMM_LOAD128(opnd1);
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();
    uint8_t imm = ir1_opnd_uimm(opnd2);

    latxs_append_ir2_opnd3(LISA_VXOR_V, &temp1, &temp1, &temp1);
    latxs_append_ir2_opnd3(LISA_VXOR_V, &temp2, &temp2, &temp2);
    if(imm & 0x10){
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &temp1, &dest, 0x00);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &temp2, &src1, 0x00);
    }
    if(imm & 0x20){
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &temp1, &dest, 0x11);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &temp2, &src1, 0x11);
    }

    latxs_append_ir2_opnd3(LISA_VFMUL_D, &temp1, &temp1, &temp2);
    latxs_tr_gen_simd_fpe_check_t(pir1, NULL);
    latxs_append_ir2_opnd3(LISA_VPACKOD_D, &temp2, &temp1, &temp1);
    latxs_append_ir2_opnd3(LISA_VPACKEV_D, &temp1, &temp1, &temp1);
    latxs_append_ir2_opnd3(LISA_VFADD_D, &temp1, &temp1, &temp2);
    latxs_tr_gen_simd_fpe_check_t(pir1, NULL);

    IR2_OPND dest_t;
    if (option_sigfpe) {
        dest_t = latxs_ra_alloc_ftemp();
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &dest_t, &dest, &dest);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &dest_t, &dest, &dest);
        }
    }

    if (option_enable_lasx) {
        latxs_append_ir2_opnd3(LISA_XVXOR_V, &dest, &dest, &dest);
    } else {
        latxs_append_ir2_opnd3(LISA_VXOR_V, &dest, &dest, &dest);
    }
    if(imm & 0x1){
        if (option_enable_lasx) {
            latxs_append_ir2_opnd2i(LISA_XVEXTRINS_D, &dest, &temp1, 0x00);
        } else {
            latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &dest, &temp1, 0x00);
        }
    }
    if(imm & 0x2){
        if (option_enable_lasx) {
            latxs_append_ir2_opnd2i(LISA_XVEXTRINS_D, &dest, &temp1, 0x11);
        } else {
            latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &dest, &temp1, 0x11);
        }
    }
    if (option_enable_lasx) {
        latxs_append_ir2_opnd2i(LISA_XVINSGR2VR_D, &dest, &latxs_zero_ir2_opnd, 2);
        latxs_append_ir2_opnd2i(LISA_XVINSGR2VR_D, &dest, &latxs_zero_ir2_opnd, 3);
    }
    latxs_tr_gen_simd_fpe_check_t(pir1, &dest_t);
    return true;
}

bool latxs_translate_movntdqa(IR1_INST *pir1)
{
    return latxs_translate_movaps(pir1);
}

bool latxs_translate_blendpd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);
    uint8_t imm = ir1_opnd_uimm(opnd2);

    if(imm & 0x1)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &dest, &src, VEXTRINS_IMM_4_0(0, 0));
    if(imm & 0x2)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &dest, &src, VEXTRINS_IMM_4_0(1, 1));

    return true;
}

bool latxs_translate_blendps(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    lsassert(ir1_opnd_is_xmm(opnd0));

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);
    uint8_t imm = ir1_opnd_uimm(opnd2);

    if(imm & 0x1)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &src, VEXTRINS_IMM_4_0(0, 0));
    if(imm & 0x2)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &src, VEXTRINS_IMM_4_0(1, 1));
    if(imm & 0x4)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &src, VEXTRINS_IMM_4_0(2, 2));
    if(imm & 0x8)
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &src, VEXTRINS_IMM_4_0(3, 3));

    return true;
}

bool latxs_translate_pblendvb(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND dest = XMM_LOAD128(opnd0);
    IR2_OPND src = XMM_LOAD128(opnd1);
    IR2_OPND mask = latxs_ra_alloc_xmm(0);
    IR2_OPND temp = latxs_ra_alloc_ftemp();

    latxs_append_ir2_opnd2i(LISA_VSLTI_B, &temp, &mask, 0);
    latxs_append_ir2_opnd4(LISA_VBITSEL_V, &dest, &dest, &src, &temp);
    return true; 
}

bool latxs_translate_mpsadbw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    IR2_OPND dest = latxs_load_freg128_from_ir1(opnd0);
    IR2_OPND src = latxs_load_freg128_from_ir1(opnd1);
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();
    IR2_OPND temp3 = latxs_ra_alloc_ftemp();
    uint8_t imm = ir1_opnd_uimm(opnd2);

    //la_vreplvei_w(temp1, src, imm & 3);
    latxs_append_ir2_opnd2i(LISA_VREPLVEI_W, &temp1, &src, imm & 3);
    //la_vmepatmsk_v(temp2, 1, imm & 0x4);
    latxs_append_ir2_opnd1ii(LISA_VMEPATMSK_V, &temp2, 1, imm & 0x4);
    //la_vmepatmsk_v(temp3, 2, imm & 0x4);
    latxs_append_ir2_opnd1ii(LISA_VMEPATMSK_V, &temp3, 2, imm & 0x4);
    //la_vshuf_b(temp2, dest, dest, temp2);
    latxs_append_ir2_opnd4(LISA_VSHUF_B, &temp2, &dest, &dest, &temp2);
    //la_vshuf_b(temp3, dest, dest, temp3);
    latxs_append_ir2_opnd4(LISA_VSHUF_B, &temp3, &dest, &dest, &temp3);
    //la_vabsd_bu(temp2, temp2, temp1);
    latxs_append_ir2_opnd3(LISA_VABSD_BU, &temp2, &temp2, &temp1);
    //la_vabsd_bu(temp3, temp3, temp1);
    latxs_append_ir2_opnd3(LISA_VABSD_BU, &temp3, &temp3, &temp1);
    //la_vhaddw_hu_bu(temp2, temp2, temp2);
    latxs_append_ir2_opnd3(LISA_VHADDW_HU_BU, &temp2, &temp2, &temp2);
    //la_vhaddw_wu_hu(temp2, temp2, temp2);
    latxs_append_ir2_opnd3(LISA_VHADDW_WU_HU, &temp2, &temp2, &temp2);
    //la_vhaddw_hu_bu(temp3, temp3, temp3);
    latxs_append_ir2_opnd3(LISA_VHADDW_HU_BU, &temp3, &temp3, &temp3);
    //la_vhaddw_wu_hu(dest, temp3, temp3);
    latxs_append_ir2_opnd3(LISA_VHADDW_WU_HU, &dest, &temp3, &temp3);
    //la_vsrlni_h_w(dest, temp2, 0);
    latxs_append_ir2_opnd2i(LISA_VSRLNI_H_W, &dest, &temp2, 0);
    return true;
}

bool latxs_translate_phminposuw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR1_OPND * opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND * opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND dest = latxs_load_freg128_from_ir1(opnd0);
    IR2_OPND src = latxs_load_freg128_from_ir1(opnd1);
    IR2_OPND temp = latxs_ra_alloc_ftemp();
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();

    //la_xvori_b(temp, src, 0x0);
    if (option_enable_lasx) {
        latxs_append_ir2_opnd2i(LISA_XVORI_B, &temp, &src, 0);
    } else {
        latxs_append_ir2_opnd2i(LISA_VORI_B, &temp, &src, 0);
    }
    //la_vsrli_d(temp, temp, 0x10);
    latxs_append_ir2_opnd2i(LISA_VSRLI_D, &temp, &temp, 0x10);
    //la_vmin_hu(temp, temp, src);
    latxs_append_ir2_opnd3(LISA_VMIN_HU, &temp, &temp, &src);
    //la_vsrli_d(temp, temp, 0x10);
    latxs_append_ir2_opnd2i(LISA_VSRLI_D, &temp, &temp, 0x10);
    //la_vmin_hu(temp, temp, src);
    latxs_append_ir2_opnd3(LISA_VMIN_HU, &temp, &temp, &src);
    //la_vsrli_d(temp, temp, 0x10);
    latxs_append_ir2_opnd2i(LISA_VSRLI_D, &temp, &temp, 0x10);
    //la_vmin_hu(temp, temp, src);
    latxs_append_ir2_opnd3(LISA_VMIN_HU, &temp, &temp, &src);

    //la_xvpickve_d(temp2, temp, 1);
    if (option_enable_lasx) {
        latxs_append_ir2_opnd2i(LISA_XVPICKVE_D, &temp2, &temp, 1);
    } else {
        latxs_append_ir2_opnd2i(LISA_VANDI_B, &temp2, &temp2, 0);
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &temp2, &temp, 1);
    }
    //la_vmin_hu(temp, temp, temp2);
    latxs_append_ir2_opnd3(LISA_VMIN_HU, &temp, &temp, &temp2);
    //la_xvreplve0_h(temp, temp);
    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_XVREPLVE0_H, &temp, &temp);
    } else {
        latxs_append_ir2_opnd2i(LISA_VREPLVEI_H, &temp, &temp, 0);
    }
    //la_vseq_h(temp1, src, temp);
    latxs_append_ir2_opnd3(LISA_VSEQ_H, &temp1, &src, &temp);
    //la_vfrstpi_h(temp2, temp1, 0);
    latxs_append_ir2_opnd2i(LISA_VFRSTPI_H, &temp2, &temp1, 0);
    //la_vpackev_h(dest, temp2, temp);
    latxs_append_ir2_opnd3(LISA_VPACKEV_H, &dest, &temp2, &temp);
    //la_xvpickve_w(dest, dest, 0);
    if (option_enable_lasx) {
        latxs_append_ir2_opnd2i(LISA_XVPICKVE_W, &dest, &dest, 0);
    } else {
        latxs_append_ir2_opnd2i(LISA_VINSGR2VR_D, &dest, &latxs_zero_ir2_opnd, 1);
        latxs_append_ir2_opnd2i(LISA_VINSGR2VR_W, &dest, &latxs_zero_ir2_opnd, 1);
    }
    return true;
}
