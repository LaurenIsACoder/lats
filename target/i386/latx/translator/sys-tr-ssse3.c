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
#include "latx-sys-tr-ssse3.h"

void latxs_sys_ssse3_register_ir1(void)
{
    // SSSE3
    latxs_register_ir1(X86_INS_PALIGNR);
    latxs_register_ir1(X86_INS_PSHUFB);
    latxs_register_ir1(X86_INS_PSIGNB);
    latxs_register_ir1(X86_INS_PSIGNW);
    latxs_register_ir1(X86_INS_PSIGND);
    latxs_register_ir1(X86_INS_PHADDD);
    latxs_register_ir1(X86_INS_PHADDW);
    latxs_register_ir1(X86_INS_PHADDSW);
    latxs_register_ir1(X86_INS_PHSUBD);
    latxs_register_ir1(X86_INS_PHSUBW);
    latxs_register_ir1(X86_INS_PHSUBSW);
    latxs_register_ir1(X86_INS_PABSB);
    latxs_register_ir1(X86_INS_PABSW);
    latxs_register_ir1(X86_INS_PABSD);
    latxs_register_ir1(X86_INS_PMADDUBSW);
    latxs_register_ir1(X86_INS_PMULHRSW);

    // SSE3
    latxs_register_ir1(X86_INS_HADDPD);
    latxs_register_ir1(X86_INS_HADDPS);
    latxs_register_ir1(X86_INS_HSUBPD);
    latxs_register_ir1(X86_INS_HSUBPS);
    latxs_register_ir1(X86_INS_MOVSHDUP);
    latxs_register_ir1(X86_INS_MOVSLDUP);
    latxs_register_ir1(X86_INS_MOVDDUP);
    latxs_register_ir1(X86_INS_ADDSUBPS);
    latxs_register_ir1(X86_INS_ADDSUBPD);
}

#define XMM_REG(o) ir1_opnd_base_reg_num(o)
#define XMM_LOAD128(o) latxs_load_freg128_from_ir1(o)
#define XMM_LOADFP1(o) latxs_load_freg_from_ir1_1(o, true)

#define XMM_EXCP(pir1) do {                             \
    if (latxs_tr_gen_sse_common_excp_check(pir1)) {     \
        return true;                                    \
    }                                                   \
} while (0)

bool latxs_translate_palignr(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    /* PALIGNR mm1,  mm2/m64,   imm8 */
    /* PALIGNR xmm1, xmm2/m128, imm8 */
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);

    IR2_OPND dest, src;
    uint8_t imm8 = ir1_opnd_uimm(opnd2);

    IR2_OPND tmp = latxs_ra_alloc_ftemp();

    if (ir1_opnd_is_xmm(opnd0)) {
        dest = XMM_LOAD128(opnd0);
        if (imm8 >= 32) {
            latxs_append_ir2_opnd3(LISA_VXOR_V, &dest, &dest, &dest);
        } else if (imm8 >= 16 && imm8 < 32) {
            latxs_append_ir2_opnd2i(LISA_VBSRL_V, &dest, &dest, imm8 - 16);
        } else {
            src = XMM_LOAD128(opnd1);
            if (imm8 == 0) {
                latxs_append_ir2_opnd2i(LISA_VORI_B, &dest, &src, 0);
            } else {
                latxs_append_ir2_opnd2i(LISA_VBSRL_V, &tmp, &src, imm8);
                latxs_append_ir2_opnd2i(LISA_VBSLL_V, &dest, &dest, 16 - imm8);
                latxs_append_ir2_opnd3(LISA_VOR_V, &dest, &dest, &tmp);
            }
        }
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        if (imm8 >= 16) {
            latxs_append_ir2_opnd3(LISA_VXOR_V, &dest, &dest, &dest);
        } else if (imm8 >= 8 && imm8 < 16) {
            latxs_append_ir2_opnd2i(LISA_VINSGR2VR_D, &dest,
                    &latxs_zero_ir2_opnd, 0x1);
            latxs_append_ir2_opnd2i(LISA_VBSRL_V, &dest, &dest, imm8 - 8);
        } else {
            src = XMM_LOADFP1(opnd1);
            if (imm8 == 0) {
                latxs_append_ir2_opnd2i(LISA_VORI_B, &dest, &src, 0);
            } else {
                latxs_append_ir2_opnd2i(LISA_VORI_B, &tmp, &src, 0);
                if (option_enable_lasx) {
                    latxs_append_ir2_opnd2i(LISA_XVINSVE0_D, &tmp, &dest, 0x1);
                } else {
                    latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &tmp, &dest, 0x1 << 4);
                }
                latxs_append_ir2_opnd2i(LISA_VBSRL_V, &dest, &tmp, imm8);
            }
        }
    }

    return true;
}

bool latxs_translate_pshufb(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    /* PSHUFB mm1,  mm2/m64   */
    /* PSHUFB xmm1, xmm2/m128 */
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND dest, src;
    IR2_OPND vzero = latxs_ra_alloc_ftemp();
    IR2_OPND src_t1 = latxs_ra_alloc_ftemp();
    IR2_OPND src_t2 = latxs_ra_alloc_ftemp();

    latxs_append_ir2_opnd2(LISA_VREPLGR2VR_D, &vzero, &latxs_zero_ir2_opnd);

    if (ir1_opnd_is_xmm(opnd0)) {
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
        latxs_append_ir2_opnd2i(LISA_VANDI_B, &src_t2, &src, 0xf);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
        latxs_append_ir2_opnd2i(LISA_VANDI_B, &src_t2, &src, 0x7);
    }

    latxs_append_ir2_opnd2i(LISA_VANDI_B, &src_t1, &src, 0x80);
    latxs_append_ir2_opnd2i(LISA_VSRLI_B, &src_t1, &src_t1, 3);
    latxs_append_ir2_opnd3(LISA_VADD_B, &src_t1, &src_t2, &src_t1);
    latxs_append_ir2_opnd4(LISA_VSHUF_B, &dest, &vzero, &dest, &src_t1);

    latxs_ra_free_temp(&vzero);
    latxs_ra_free_temp(&src_t1);
    latxs_ra_free_temp(&src_t2);

    return true;
}

bool latxs_translate_haddpd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 0)));
    IR2_OPND dest = XMM_LOAD128(ir1_get_opnd(pir1, 0));
    IR2_OPND src = XMM_LOAD128(ir1_get_opnd(pir1, 1)); 
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();
    latxs_append_ir2_opnd3(LISA_VPICKEV_D, &temp1, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VPICKOD_D, &temp2, &src, &dest);

    IR2_OPND dest_t;
    if (option_sigfpe) {
        dest_t = latxs_ra_alloc_ftemp();
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &dest_t, &dest, &dest);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &dest_t, &dest, &dest);
        }
    }

    latxs_append_ir2_opnd3(LISA_VFADD_D, &dest, &temp1, &temp2);
    
    latxs_tr_gen_simd_fpe_check_t(pir1, &dest_t);
    latxs_ra_free_temp(&temp2);
    latxs_ra_free_temp(&temp1);

    return true;
}

bool latxs_translate_haddps(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 0)));
    IR2_OPND dest = XMM_LOAD128(ir1_get_opnd(pir1, 0));
    IR2_OPND src = XMM_LOAD128(ir1_get_opnd(pir1, 1)); 
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();
    /**
     * origin:
     * s3, s2, s1, s0
     * d3, d2, d1, d0
     *
     * after pick:
     * temp1:   s2, s0, d2, d0
     * temp2:   s3, s1, d3, d1
     */
    latxs_append_ir2_opnd3(LISA_VPICKEV_W, &temp1, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VPICKOD_W, &temp2, &src, &dest);

    IR2_OPND dest_t;
    if (option_sigfpe) {
        dest_t = latxs_ra_alloc_ftemp();
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &dest_t, &dest, &dest);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &dest_t, &dest, &dest);
        }
    }

    latxs_append_ir2_opnd3(LISA_VFADD_S, &dest, &temp1, &temp2);
    
    latxs_tr_gen_simd_fpe_check_t(pir1, &dest_t);
    latxs_ra_free_temp(&temp2);
    latxs_ra_free_temp(&temp1);

    return true;
}

bool latxs_translate_hsubpd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 0)));
    IR2_OPND dest = XMM_LOAD128(ir1_get_opnd(pir1, 0));
    IR2_OPND src = XMM_LOAD128(ir1_get_opnd(pir1, 1)); 
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();
    latxs_append_ir2_opnd3(LISA_VPICKEV_D, &temp1, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VPICKOD_D, &temp2, &src, &dest);

    IR2_OPND dest_t;
    if (option_sigfpe) {
        dest_t = latxs_ra_alloc_ftemp();
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &dest_t, &dest, &dest);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &dest_t, &dest, &dest);
        }
    }

    latxs_append_ir2_opnd3(LISA_VFSUB_D, &dest, &temp1, &temp2);
    
    latxs_tr_gen_simd_fpe_check_t(pir1, &dest_t);
    latxs_ra_free_temp(&temp2);
    latxs_ra_free_temp(&temp1);

    return true;
}

bool latxs_translate_hsubps(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 0)));
    IR2_OPND dest = XMM_LOAD128(ir1_get_opnd(pir1, 0));
    IR2_OPND src = XMM_LOAD128(ir1_get_opnd(pir1, 1)); 
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();
    latxs_append_ir2_opnd3(LISA_VPICKEV_W, &temp1, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VPICKOD_W, &temp2, &src, &dest);

    IR2_OPND dest_t;
    if (option_sigfpe) {
        dest_t = latxs_ra_alloc_ftemp();
        if (option_enable_lasx) {
            latxs_append_ir2_opnd3(LISA_XVOR_V, &dest_t, &dest, &dest);
        } else {
            latxs_append_ir2_opnd3(LISA_VOR_V, &dest_t, &dest, &dest);
        }
    }

    latxs_append_ir2_opnd3(LISA_VFSUB_S, &dest, &temp1, &temp2);
    
    latxs_tr_gen_simd_fpe_check_t(pir1, &dest_t);
    latxs_ra_free_temp(&temp2);
    latxs_ra_free_temp(&temp1);

    return true;
}

bool latxs_translate_phaddd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dest;
    IR2_OPND src;
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();

    if (ir1_opnd_is_xmm(opnd0)) {                             
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd3(LISA_VPICKEV_W, &temp1, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VPICKOD_W, &temp2, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VADD_W, &dest, &temp1, &temp2);
    if (!ir1_opnd_is_xmm(opnd0)) {
        latxs_append_ir2_opnd2i(LISA_VSHUF4I_W, &dest, &dest, 0xd8);
    }
    latxs_ra_free_temp(&temp2);
    latxs_ra_free_temp(&temp1);

    return true;
}

bool latxs_translate_phaddw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dest;
    IR2_OPND src;
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();

    if (ir1_opnd_is_xmm(opnd0)) {                             
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd3(LISA_VPICKEV_H, &temp1, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VPICKOD_H, &temp2, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VADD_H, &dest, &temp1, &temp2);
    if (!ir1_opnd_is_xmm(opnd0)) {
        latxs_append_ir2_opnd2i(LISA_VSHUF4I_W, &dest, &dest, 0xd8);
    }
    latxs_ra_free_temp(&temp2);
    latxs_ra_free_temp(&temp1);

    return true;
}

bool latxs_translate_phaddsw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dest;
    IR2_OPND src;
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();

    if (ir1_opnd_is_xmm(opnd0)) {                             
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd3(LISA_VPICKEV_H, &temp1, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VPICKOD_H, &temp2, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VSADD_H, &temp2, &temp1, &temp2);
    if (!ir1_opnd_is_xmm(opnd0)) {
        latxs_append_ir2_opnd2i(LISA_VSHUF4I_W, &temp2, &temp2, 0xd8);
    }
    latxs_append_ir2_opnd2i(LISA_VORI_B, &dest, &temp2, 0);
    latxs_ra_free_temp(&temp2);
    latxs_ra_free_temp(&temp1);

    return true;
}

bool latxs_translate_phsubd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dest;
    IR2_OPND src;
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();

    if (ir1_opnd_is_xmm(opnd0)) {                             
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd3(LISA_VPICKEV_W, &temp1, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VPICKOD_W, &temp2, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VSUB_W, &dest, &temp1, &temp2);
    if (!ir1_opnd_is_xmm(opnd0)) {
        latxs_append_ir2_opnd2i(LISA_VSHUF4I_W, &dest, &dest, 0xd8);
    }
    latxs_ra_free_temp(&temp2);
    latxs_ra_free_temp(&temp1);

    return true;
}

bool latxs_translate_phsubw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dest;
    IR2_OPND src;
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();

    if (ir1_opnd_is_xmm(opnd0)) {                             
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd3(LISA_VPICKEV_H, &temp1, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VPICKOD_H, &temp2, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VSUB_H, &dest, &temp1, &temp2);
    if (!ir1_opnd_is_xmm(opnd0)) {
        latxs_append_ir2_opnd2i(LISA_VSHUF4I_W, &dest, &dest, 0xd8);
    }
    latxs_ra_free_temp(&temp2);
    latxs_ra_free_temp(&temp1);

    return true;
}

bool latxs_translate_phsubsw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dest;
    IR2_OPND src;
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();

    if (ir1_opnd_is_xmm(opnd0)) {                             
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd3(LISA_VPICKEV_H, &temp1, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VPICKOD_H, &temp2, &src, &dest);
    latxs_append_ir2_opnd3(LISA_VSSUB_H, &dest, &temp1, &temp2);
    if (!ir1_opnd_is_xmm(opnd0)) {
        latxs_append_ir2_opnd2i(LISA_VSHUF4I_W, &dest, &dest, 0xd8);
    }
    latxs_ra_free_temp(&temp2);
    latxs_ra_free_temp(&temp1);

    return true;
}

bool latxs_translate_movshdup(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 0))); 
    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 1)) ||
                     ir1_opnd_is_mem(ir1_get_opnd(pir1, 1)));

    IR2_OPND dest = XMM_LOAD128(ir1_get_opnd(pir1, 0));
    IR2_OPND src = XMM_LOAD128(ir1_get_opnd(pir1, 1));

    if (option_enable_lasx) {
        latxs_append_ir2_opnd3(LISA_XVPACKOD_W, &dest, &src, &src);
    } else {
        latxs_append_ir2_opnd3(LISA_VPACKOD_W, &dest, &src, &src);
    }
    return true;
}

bool latxs_translate_movsldup(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 0))); 
    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 1)) ||
                     ir1_opnd_is_mem(ir1_get_opnd(pir1, 1)));

    IR2_OPND dest = XMM_LOAD128(ir1_get_opnd(pir1, 0));
    IR2_OPND src = XMM_LOAD128(ir1_get_opnd(pir1, 1));

    if (option_enable_lasx) {
        latxs_append_ir2_opnd3(LISA_XVPACKEV_W, &dest, &src, &src);
    } else {
        latxs_append_ir2_opnd3(LISA_VPACKEV_W, &dest, &src, &src);
    }
    return true;
}

bool latxs_translate_movddup(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    IR2_OPND src_lo = XMM_LOAD128(ir1_get_opnd(pir1, 1));
    IR2_OPND dest = XMM_LOAD128(ir1_get_opnd(pir1, 0));
    if (option_enable_lasx) {
        latxs_append_ir2_opnd2(LISA_XVREPLVE0_D, &dest, &src_lo);
    } else {
        latxs_append_ir2_opnd2(LISA_VREPLVEI_D, &dest, &src_lo);
    }

    return true;
}

bool latxs_translate_pabsb(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND vzero = latxs_ra_alloc_ftemp();

    IR2_OPND src;
    IR2_OPND dest;

    if (ir1_opnd_is_xmm(opnd0)) {
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd2(LISA_VREPLGR2VR_D, &vzero, &latxs_zero_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_VABSD_B, &dest, &src, &vzero);

    return true;
}

bool latxs_translate_pabsw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND vzero = latxs_ra_alloc_ftemp();

    IR2_OPND src;
    IR2_OPND dest;

    if (ir1_opnd_is_xmm(opnd0)) {
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd2(LISA_VREPLGR2VR_D, &vzero, &latxs_zero_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_VABSD_H, &dest, &src, &vzero);

    return true;
}

bool latxs_translate_pabsd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND vzero = latxs_ra_alloc_ftemp();

    IR2_OPND src;
    IR2_OPND dest;

    if (ir1_opnd_is_xmm(opnd0)) {
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd2(LISA_VREPLGR2VR_D, &vzero, &latxs_zero_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_VABSD_W, &dest, &src, &vzero);

    return true;
}

bool latxs_translate_pmaddubsw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dest;                    
    IR2_OPND src;
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();
    IR2_OPND temp3 = latxs_ra_alloc_ftemp();
    IR2_OPND temp4 = latxs_ra_alloc_ftemp();
    IR2_OPND temp5 = latxs_ra_alloc_ftemp();
    IR2_OPND itmp = latxs_ra_alloc_itemp(); 

    if (ir1_opnd_is_xmm(opnd0)) {
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    /* unsigned dest * signed src */      

    /* U dest * abs(src) */
    latxs_append_ir2_opnd2(LISA_VREPLGR2VR_D, &temp1, &latxs_zero_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_VABSD_B, &temp3, &src, &temp1);
    latxs_append_ir2_opnd3(LISA_VMADDWEV_H_BU, &temp1, &dest, &temp3);
    latxs_append_ir2_opnd2(LISA_VREPLGR2VR_D, &temp2, &latxs_zero_ir2_opnd);
    latxs_append_ir2_opnd3(LISA_VMADDWOD_H_BU, &temp2, &dest, &temp3);

    latxs_append_ir2_opnd2i(LISA_ORI, &itmp, &latxs_zero_ir2_opnd, 0x1);
    latxs_append_ir2_opnd2(LISA_VREPLGR2VR_B, &temp3, &itmp);
    latxs_append_ir2_opnd3(LISA_VSIGNCOV_B, &temp4, &src, &temp3);

    latxs_append_ir2_opnd3(LISA_VMULWEV_H_B, &temp5, &temp4, &temp3);
    latxs_append_ir2_opnd3(LISA_VMULWOD_H_B, &temp3, &temp4, &temp3);

    latxs_append_ir2_opnd3(LISA_VMUL_H, &temp1, &temp1, &temp5);
    latxs_append_ir2_opnd3(LISA_VMUL_H, &temp2, &temp2, &temp3);
    latxs_append_ir2_opnd3(LISA_VSADD_H, &dest, &temp2, &temp1);

    return true;
}

bool latxs_translate_psignb(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src;
    IR2_OPND dest;

    if (ir1_opnd_is_xmm(opnd0)) {
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd3(LISA_VSIGNCOV_B, &dest, &src, &dest);

    return true;
}

bool latxs_translate_psignw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src;
    IR2_OPND dest;

    if (ir1_opnd_is_xmm(opnd0)) {
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd3(LISA_VSIGNCOV_H, &dest, &src, &dest);

    return true;
}

bool latxs_translate_psignd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND src;
    IR2_OPND dest;

    if (ir1_opnd_is_xmm(opnd0)) {
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
    } else {
        /* mmx */
        dest = XMM_LOADFP1(opnd0);
        src = XMM_LOADFP1(opnd1);
    }

    latxs_append_ir2_opnd3(LISA_VSIGNCOV_W, &dest, &src, &dest);

    return true;
}

bool latxs_translate_addsubps(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 0)));
    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 1)) ||
             ir1_opnd_is_mem(ir1_get_opnd(pir1, 1)));

    IR2_OPND dest = XMM_LOAD128(ir1_get_opnd(pir1, 0));
    IR2_OPND src = XMM_LOAD128(ir1_get_opnd(pir1, 1));
    IR2_OPND temp_dest_sub = latxs_ra_alloc_ftemp();
    IR2_OPND ftemp_src_add = latxs_ra_alloc_ftemp();
    IR2_OPND ftemp_src_sub = latxs_ra_alloc_ftemp();
    IR2_OPND ftemp_dest_add = latxs_ra_alloc_ftemp();
    IR2_OPND ftemp_dest_sub = latxs_ra_alloc_ftemp();

    /**
     * The two should not interfere with each other When          
     * calculating addition or subtraction, or unexpected FPE     
     * would be triggered. See VPERMI in Volume 2 of LOONGARCH for
     * more informations.
     */
    latxs_append_ir2_opnd2i(LISA_VBSLL_V, &ftemp_src_add, &src, 0);
    latxs_append_ir2_opnd2i(LISA_VBSLL_V, &ftemp_src_sub, &src, 0);
    latxs_append_ir2_opnd2i(LISA_VBSLL_V, &ftemp_dest_add, &dest, 0);
    latxs_append_ir2_opnd2i(LISA_VBSLL_V, &ftemp_dest_sub, &dest, 0);

    latxs_append_ir2_opnd2i(LISA_VPERMI_W, &ftemp_src_add, &src, 0xf5);
    latxs_append_ir2_opnd2i(LISA_VPERMI_W, &ftemp_dest_add, &dest, 0xf5);
    latxs_append_ir2_opnd2i(LISA_VPERMI_W, &ftemp_src_sub, &src, 0xa0);
    latxs_append_ir2_opnd2i(LISA_VPERMI_W, &ftemp_dest_sub, &dest, 0xa0);

    latxs_append_ir2_opnd3(LISA_VFSUB_S, &temp_dest_sub, &ftemp_dest_sub, &ftemp_src_sub);
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

    latxs_append_ir2_opnd3(LISA_VFADD_S, &dest, &ftemp_dest_add, &ftemp_src_add);

    latxs_tr_gen_simd_fpe_check_t(pir1, &dest_t);
    /*
     * Pick temp_sub data to [0:31] [64:95]
     */
    if (option_enable_lasx) {
        latxs_append_ir2_opnd2i(LISA_XVINSVE0_W, &dest, &temp_dest_sub, 0);
    } else {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &temp_dest_sub, 0);
    }
    latxs_append_ir2_opnd2i(LISA_VBSRL_V, &temp_dest_sub, &temp_dest_sub, 8);
    if (option_enable_lasx) {
        latxs_append_ir2_opnd2i(LISA_XVINSVE0_W, &dest, &temp_dest_sub, 2);
    } else {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_W, &dest, &temp_dest_sub, 0x2 << 4);
    }

    latxs_ra_free_temp(&temp_dest_sub);
    latxs_ra_free_temp(&ftemp_src_add);
    latxs_ra_free_temp(&ftemp_src_sub);
    latxs_ra_free_temp(&ftemp_dest_add);
    latxs_ra_free_temp(&ftemp_dest_sub);

    return true;
}

bool latxs_translate_addsubpd(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 0)));
    lsassert(ir1_opnd_is_xmm(ir1_get_opnd(pir1, 1)) ||
             ir1_opnd_is_mem(ir1_get_opnd(pir1, 1)));

    IR2_OPND dest = XMM_LOAD128(ir1_get_opnd(pir1, 0));
    IR2_OPND src = XMM_LOAD128(ir1_get_opnd(pir1, 1));
    IR2_OPND temp_dest_sub = latxs_ra_alloc_ftemp();
    IR2_OPND ftemp_src_add = latxs_ra_alloc_ftemp();
    IR2_OPND ftemp_src_sub = latxs_ra_alloc_ftemp();
    IR2_OPND ftemp_dest_add = latxs_ra_alloc_ftemp();
    IR2_OPND ftemp_dest_sub = latxs_ra_alloc_ftemp();

    latxs_append_ir2_opnd2i(LISA_VBSLL_V, &ftemp_src_add, &src, 0);
    latxs_append_ir2_opnd2i(LISA_VBSLL_V, &ftemp_src_sub, &src, 0);
    latxs_append_ir2_opnd2i(LISA_VBSLL_V, &ftemp_dest_add, &dest, 0);
    latxs_append_ir2_opnd2i(LISA_VBSLL_V, &ftemp_dest_sub, &dest, 0);

    latxs_append_ir2_opnd2i(LISA_VPERMI_W, &ftemp_src_add, &src, 0xee);
    latxs_append_ir2_opnd2i(LISA_VPERMI_W, &ftemp_dest_add, &dest, 0xee);
    latxs_append_ir2_opnd2i(LISA_VPERMI_W, &ftemp_src_sub, &src, 0x44);
    latxs_append_ir2_opnd2i(LISA_VPERMI_W, &ftemp_dest_sub, &dest, 0x44);

    latxs_append_ir2_opnd3(LISA_VFSUB_D, &temp_dest_sub, &ftemp_dest_sub, &ftemp_src_sub);
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

    latxs_append_ir2_opnd3(LISA_VFADD_D, &dest, &ftemp_dest_add, &ftemp_src_add);
    latxs_tr_gen_simd_fpe_check_t(pir1, &dest_t);
    if (option_enable_lasx) {
        latxs_append_ir2_opnd2i(LISA_XVINSVE0_D, &dest, &temp_dest_sub, 0);
    } else {
        latxs_append_ir2_opnd2i(LISA_VEXTRINS_D, &dest, &temp_dest_sub, 0);
    }

    latxs_ra_free_temp(&temp_dest_sub);
    latxs_ra_free_temp(&ftemp_src_add);
    latxs_ra_free_temp(&ftemp_src_sub);
    latxs_ra_free_temp(&ftemp_dest_add);
    latxs_ra_free_temp(&ftemp_dest_sub);

    return true;
}

bool latxs_translate_pmulhrsw(IR1_INST *pir1)
{
    XMM_EXCP(pir1);
    if (latxs_tr_gen_excp_check(pir1)) return true;

    latxs_enter_mmx(pir1);

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);
    IR2_OPND dest;
    IR2_OPND src;
    IR2_OPND temp1 = latxs_ra_alloc_ftemp();
    IR2_OPND temp2 = latxs_ra_alloc_ftemp();

    if (ir1_opnd_is_xmm(opnd0)) {                             
        /* xmm */
        dest = XMM_LOAD128(opnd0);
        src = XMM_LOAD128(opnd1);
        latxs_append_ir2_opnd3(LISA_VMULWEV_W_H, &temp1, &dest, &src);
        latxs_append_ir2_opnd3(LISA_VMULWOD_W_H, &temp2, &dest, &src);
        latxs_append_ir2_opnd2i(LISA_VSRAI_W, &temp1, &temp1, 14);
        latxs_append_ir2_opnd2i(LISA_VSRAI_W, &temp2, &temp2, 14);
        latxs_append_ir2_opnd2i(LISA_VADDI_WU, &temp1, &temp1, 1);
        latxs_append_ir2_opnd2i(LISA_VADDI_WU, &temp2, &temp2, 1);
        latxs_append_ir2_opnd2i(LISA_VSRANI_H_W, &temp2, &temp1, 1);
        latxs_append_ir2_opnd2i(LISA_VSHUF4I_W, &temp2, &temp2, 0xd8);
        latxs_append_ir2_opnd2i(LISA_VSHUF4I_H, &dest, &temp2, 0xd8);
        return true;
    }
    /* mmx */
    dest = XMM_LOADFP1(opnd0);
    src = XMM_LOADFP1(opnd1);
    latxs_append_ir2_opnd3(LISA_VMULWEV_W_H, &temp1, &dest, &src);
    latxs_append_ir2_opnd3(LISA_VMULWOD_W_H, &temp2, &dest, &src);
    latxs_append_ir2_opnd2i(LISA_VSRAI_W, &temp1, &temp1, 14);
    latxs_append_ir2_opnd2i(LISA_VSRAI_W, &temp2, &temp2, 14);
    latxs_append_ir2_opnd2i(LISA_VADDI_WU, &temp1, &temp1, 1);
    latxs_append_ir2_opnd2i(LISA_VADDI_WU, &temp2, &temp2, 1);
    latxs_append_ir2_opnd2i(LISA_VSRANI_H_W, &temp2, &temp1, 1);
    latxs_append_ir2_opnd2i(LISA_VSHUF4I_W, &temp2, &temp2, 0xd8);
    latxs_append_ir2_opnd2i(LISA_VSHUF4I_H, &dest, &temp2, 0xd8);
    return true;
}
