/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "reg-alloc.h"
#include "lsenv.h"
#include "latx-options.h"
#include "translate.h"
#include "latxs-imm-cache.h"

void latxs_tr_ra_init(TRANSLATION_DATA *td)
{
    td->itemp_mask = 0;
    td->ftemp_mask = 0;
    td->ptn_itemp_mask = 0;
}


/*
 * Integer Register Mapping of LATS x86-64
 * -------------------------------------------------------------------
 *  0 zero
 *  1 ra
 *  2 tp
 *  3 sp
 * -------------------------------------------------------------------
 *  4 a0/v0
 *  5 a1/v1
 *  6 a2
 *  7 a3
 * -------------------------------------------------------------------
 *  8 a4    TMP    : temp registers
 *  9 a5    TMP    : temp registers
 * 10 a6    TB     : TB address in eob
 * 11 a7    TMP    : temp registers
 * -------------------------------------------------------------------
 * 12 t0    R8     : x86 mapping registers r8
 * 13 t1    R9     : x86 mapping registers r9
 * 14 t2    R10    : x86 mapping registers r10
 * 15 t3    R11    : x86 mapping registers r11
 * -------------------------------------------------------------------
 * 16 t4    R12    : x86 mapping registers r12
 * 17 t5    R13    : x86 mapping registers r13
 * 18 t6    R14    : x86 mapping registers r14
 * 19 t7    R15    : x86 mapping registers r15
 * -------------------------------------------------------------------
 * 20 t8    TMP    : temp registers
 * 21 x     TMP    : temp registers
 * 22 fp    TMP    : temp registers
 * 23 s0    ENV    : address(HVA) of CPUX86State
 * -------------------------------------------------------------------
 * 24 s1    RAX    : x86 mapping registers rax
 * 25 s2    RCX    : x86 mapping registers rcx
 * 26 s3    RDX    : x86 mapping registers rdx
 * 27 s4    RBX    : x86 mapping registers rbx
 * -------------------------------------------------------------------
 * 28 s5    RSP    : x86 mapping registers rsp
 * 29 s6    RBP    : x86 mapping registers rbp
 * 30 s7    RSI    : x86 mapping registers rsi
 * 31 s8    RDI    : x86 mapping registers rdi
 * -------------------------------------------------------------------
 */

/* allocate x86 register */

IR2_OPND latxs_ra_alloc_gpr(int gpr_num)
{
    lsassert(gpr_num >= 0 && gpr_num < CPU_NB_REGS);

    /* EAX - EDI => S1 - S8 */
    /* R8  - R15 => T0 - T7 */
    static const int8_t latxs_rai_imap_gpr[CPU_NB_REGS] = {
        24, 25, 26, 27, 28, 29, 30, 31,
        12, 13, 14, 15, 16, 17, 18, 19
    };

    IR2_OPND opnd;
    latxs_ir2_opnd_build(&opnd, IR2_OPND_GPR, latxs_rai_imap_gpr[gpr_num]);

    return opnd;
}

#define LATXS_IR2_VREG_NUM 6
IR2_OPND latxs_ra_alloc_vreg(int vr_num)
{
    lsassert(vr_num >= 0 && vr_num < LATXS_IR2_VREG_NUM);

    /*
     * bit 0: guest base, not used in system mode
     * bit 1: last executed tb, mapped to $10, a6; register not always valid
     * bit 2: next x86 addr, mapped to $11; registerk not always valid
     * bit 3: top_bias, not mapped to register
     * bit 4: shadow stack, not used in system mode
     * bit 5: FastCS mask, not mapped to resgiter, not supported yet
     */

    static const int8_t latxs_rai_imap_vr[LATXS_IR2_VREG_NUM] = {
    0, 10, 0, 0, 0, 0};

    int reg = latxs_rai_imap_vr[vr_num];
    lsassert(reg);

    IR2_OPND opnd = latxs_ir2_opnd_new(IR2_OPND_GPR, reg);
    return opnd;
}

IR2_OPND latxs_ra_alloc_mmx(int mmx_num)
{
    static const int8_t latxs_rai_fmap_mmx[8] = {
    0, 1, 2, 3, 4, 5, 6, 7};

    return latxs_ir2_opnd_new(IR2_OPND_FPR, latxs_rai_fmap_mmx[mmx_num]);
}

IR2_OPND latxs_ra_alloc_st(int st_num)
{
    lsassert(!option_soft_fpu);
    static const int8_t latxs_rai_fmap_mmx[8] = {
    0, 1, 2, 3, 4, 5, 6, 7};

    int fpr_num = st_num;

    return latxs_ir2_opnd_new(IR2_OPND_FPR, latxs_rai_fmap_mmx[fpr_num]);
}

IR2_OPND latxs_ra_alloc_xmm(int num)
{
    lsassert(0 <= num && num < CPU_NB_REGS);
    return latxs_ir2_opnd_new(IR2_OPND_FPR, 16 + num);
}

IR2_OPND latxs_ra_alloc_dbt_arg1(void) { return latxs_ra_alloc_vreg(1); }

IR2_OPND latxs_ra_alloc_ptn_itemp(void)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    IR2_OPND ir2_opnd;
    ir2_opnd._type = IR2_OPND_GPR;

    /* 1. allocate physical register */
    const TEMP_REG_STATUS *p = latxs_itemp_status_default;

    int mask = td->itemp_mask | td->ptn_itemp_mask;
    int i = 0;
    while ((mask >> i) & 0x1) {
        i++;
    }

#ifdef LATXS_IMM_CACHE_ENABLE
    if (can_do_imm() && (option_imm_rip || option_imm_pattern) && imm_cache_is_using_temp(i)) {
        imm_cache_free_itemp(i);
    }
#endif

    if (i < latxs_itemp_status_num) {
        ir2_opnd.val = p[i].physical_id;
        td->ptn_itemp_mask = td->ptn_itemp_mask | (1 << i);
        return ir2_opnd;
    }

    lsassertm(0, "no more itemp to be alloc.\n");
    return ir2_opnd;
}

IR2_OPND latxs_ra_alloc_itemp(void)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    IR2_OPND ir2_opnd;
    ir2_opnd._type = IR2_OPND_GPR;

    /* 1. allocate physical register */
    const TEMP_REG_STATUS *p = latxs_itemp_status_default;

    int mask = td->itemp_mask | td->ptn_itemp_mask;
    int i = 0;
    while ((mask >> i) & 0x1) {
        i++;
    }

    if (option_instptn && i >= latxs_itemp_status_num) {
        td->ptn_itemp_mask = 0;
        mask = td->itemp_mask;
        i = 0;
        while ((mask >> i) & 0x1) {
            i++;
        }
    }

#ifdef LATXS_IMM_CACHE_ENABLE
    if (can_do_imm() && (option_imm_rip || option_imm_pattern) && imm_cache_is_using_temp(i)) {
        imm_cache_free_itemp(i);
    }
#endif

    if (i < latxs_itemp_status_num) {
        ir2_opnd.val = p[i].physical_id;
        td->itemp_mask = td->itemp_mask | (1 << i);
        return ir2_opnd;
    }

    fprintf(stderr, "%s:%d ERROR not enough itemp\n",
            __func__, __LINE__);
    g_assert_not_reached();
    return ir2_opnd;
}

IR2_OPND latxs_ra_alloc_ftemp(void)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    IR2_OPND ir2_opnd;
    ir2_opnd._type = IR2_OPND_FPR;

    /* 1. allocate physical register */
    const TEMP_REG_STATUS *p = ftemp_status_default;

    uint32_t mask = td->ftemp_mask;
    int i = 0;
    while ((mask >> i) & 0x1) {
        i++;
    }

    if (i < ftemp_status_num) {
        ir2_opnd.val = p[i].physical_id;
        td->ftemp_mask = td->ftemp_mask | (1 << i);
        return ir2_opnd;
    }

    fprintf(stderr, "%s:%d ERROR not enough ftemp\n",
            __func__, __LINE__);
    g_assert_not_reached();
    return ir2_opnd;
}

void latxs_ra_free_temp(IR2_OPND *opnd)
{
    if (!latxs_ir2_opnd_is_reg_temp(opnd)) {
        return;
    }

    TRANSLATION_DATA *td = lsenv->tr_data;

    const TEMP_REG_STATUS *ireg_map = latxs_itemp_status_default;
    const TEMP_REG_STATUS *freg_map = ftemp_status_default;

    int physical_reg_num = latxs_ir2_opnd_reg(opnd);

    int i = 0;
    switch (latxs_ir2_opnd_type(opnd)) {
    case IR2_OPND_GPR:
        for (i = 0; i < latxs_itemp_status_num; ++i) {
            if (ireg_map[i].physical_id == physical_reg_num) {
                td->itemp_mask = td->itemp_mask & ~(1 << i);
                break;
            }
        }
        break;
    case IR2_OPND_FPR:
        for (i = 0; i < ftemp_status_num; ++i) {
            if (freg_map[i].physical_id == physical_reg_num) {
                td->ftemp_mask = td->ftemp_mask & ~(1 << i);
                break;
            }
        }
        break;
    default:
        fprintf(stderr, "%s:%d ERROR free temp but not i/f/mem\n",
                __func__, __LINE__);
        g_assert_not_reached();
        break;
    }
}

inline __attribute__((always_inline)) bool latxs_ra_temp_is_free(int latxs_temp_index)
{
    int mask = lsenv->tr_data->itemp_mask;
    if (option_instptn) {
        mask = mask | lsenv->tr_data->ptn_itemp_mask;
    }
    bool r = !((mask >> latxs_temp_index) & 0x1);
    return r;
}

/* temp allocated by cache doesn't affect env->itemp_mask */
IR2_OPND latxs_ra_alloc_imm_reg(int itemp_index)
{
    if (latxs_ra_temp_is_free(itemp_index)) {
        TRANSLATION_DATA *td = lsenv->tr_data;
        td->itemp_mask = td->itemp_mask | (1 << itemp_index);
        return latxs_ir2_opnd_new(IR2_OPND_GPR,
                                  latxs_itemp_status_default[itemp_index].physical_id);
    }
    /* do not use 'latxs_ir2_opnd_new_label' which will add lsenv->label_num */
    return latxs_ir2_opnd_new(IR2_OPND_LABEL, 0);
}
