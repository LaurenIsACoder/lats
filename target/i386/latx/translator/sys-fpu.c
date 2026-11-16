/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "lsenv.h"
#include "latx-config.h"
#include "reg-alloc.h"
#include "translate.h"
#include "latx-options.h"
#include "hamt-options.h"
#include "latx-sys-hamt-dmw.h"

//#define LATX_SYS_NO_SIMD_FCSR

void latxs_tr_fpu_push(void) { latxs_tr_fpu_dec(); }
void latxs_tr_fpu_pop(void) { latxs_tr_fpu_inc(); }

void latxs_tr_fpu_inc(void)
{
    assert(option_lsfpu);
    latxs_append_ir2_opnd0(LISA_X86INCTOP);
}

void latxs_tr_fpu_dec(void)
{
    assert(option_lsfpu);
    latxs_append_ir2_opnd0(LISA_X86DECTOP);
}

void latxs_tr_gen_save_curr_top_force(void)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    td->force_curr_top_save_bak = td->force_curr_top_save;
    td->force_curr_top_save = 1;
    latxs_tr_gen_save_curr_top();
    lsenv->tr_data->force_curr_top_save = 0;
    td->force_curr_top_save = td->force_curr_top_save_bak;
}

void latxs_tr_gen_save_curr_top(void)
{
    assert(option_lsfpu);
    IR2_OPND top = latxs_ra_alloc_itemp();
    latxs_tr_save_lstop_to_env(&top);
    latxs_ra_free_temp(&top);
}

#ifdef LATX_SYS_NO_SIMD_FCSR

void latxs_set_fpu_fcsr(IR2_OPND *new_fcsr) { return; }

IR2_OPND latxs_set_fpu_fcsr_rounding_field_by_x86(void)
{
    return latxs_zero_ir2_opnd;
}

#else

#if defined(LATX_SYS_FCSR)
static __thread int simd_fcsr_saved;
/*
 * latxs_set_fpu_fcsr_rounding_field_by_x86() : set()
 * latxs_set_fpu_fcsr()                       : reset()
 *
 * RM: rounding mode
 *
 * In x86, SIMD's RM is controled by env->mxcsr
 *         FPU's  RM is controled by env->fcsr
 *
 * In LA, both are controled by LA's FCSR
 *
 * Some SIMD instructions will use env->mxcsr's RM:
 *  1> read current LA's FCSR into temp register @tmp
 *  2> call set() to set LA's FCSR RM according to env->mxcsr
 *  3> do SIMD operation
 *  4> call reset() to set LA's FCSR according to @tmp
 *
 * During step <3>, the LA's FCSR is NOT env->fcsr  (x86's FPU)
 *                                   but env->mxcsr (x86's SIMD)
 *
 * So we hack the set() and reset() function to indicate it:
 *  >> in step <2> set()   will set env->is_fscr_simd = 1
 *  >> in step <4> reset() will set env->is_fcse_simd = 0
 *
 * During context switch, if env->is_fcsr_simd is set to 1, then
 * the LA's FCSR will be saved/loaded into/from env->fscr_simd.
 *
 * TODO: what if exception occurs in step<3> ?
 * For now we simply assert it in latxs_fix_after_excp_or_int()
 */
#endif

void latxs_set_fpu_fcsr(IR2_OPND *new_fcsr)
{
#if defined(FCSR_LOAD_RM_ONLY)
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr3_ir2_opnd,
            new_fcsr);
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr1_ir2_opnd,
            &latxs_zero_ir2_opnd);
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr2_ir2_opnd,
            &latxs_zero_ir2_opnd);
#else
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr_ir2_opnd,
            new_fcsr);
#endif

#if defined(LATX_SYS_FCSR)
    assert(simd_fcsr_saved == 1);
    simd_fcsr_saved = 0;

    IR2_OPND *zero = &latxs_zero_ir2_opnd;
    IR2_OPND tmp_int = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ORI, &tmp_int, zero, 0);
    latxs_append_ir2_opnd2i(LISA_ST_W, &tmp_int, &latxs_env_ir2_opnd,
            offsetof(CPUX86State, is_fcsr_simd));
    latxs_ra_free_temp(&tmp_int);
#endif
}

IR2_OPND latxs_set_fpu_fcsr_rounding_field_by_x86(void)
{
    IR2_OPND *zero = &latxs_zero_ir2_opnd;
    IR2_OPND *fcsr = &latxs_fcsr_ir2_opnd;

    IR2_OPND temp_fcsr = latxs_ra_alloc_itemp();

    latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &temp_fcsr, fcsr);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &temp_fcsr, zero, 9, 8);

    /* save fscr in fcsr_opnd for reload */
    IR2_OPND fcsr_opnd = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_OR, &fcsr_opnd, &temp_fcsr, zero);

    /* set fcsr according to x86 MXCSR register */
    IR2_OPND temp_mxcsr = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_LD_WU, &temp_mxcsr,
            &latxs_env_ir2_opnd,
            lsenv_offset_of_mxcsr(lsenv));
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_W, &temp_mxcsr,
                                              &temp_mxcsr, 14, 13);

    IR2_OPND temp_int = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ANDI, &temp_int, &temp_mxcsr, 0x1);

    IR2_OPND label1 = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &temp_int, zero, &label1);
    latxs_append_ir2_opnd2i(LISA_XORI, &temp_mxcsr, &temp_mxcsr, 0x2);
    latxs_append_ir2_opnd1(LISA_LABEL, &label1);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_W, &temp_fcsr, &temp_mxcsr, 9, 8);
#if defined(FCSR_LOAD_RM_ONLY)
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr3_ir2_opnd,
            &temp_fcsr);
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr1_ir2_opnd,
            &latxs_zero_ir2_opnd);
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr2_ir2_opnd,
            &latxs_zero_ir2_opnd);
#else
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, fcsr, &temp_fcsr);
#endif

    latxs_ra_free_temp(&temp_fcsr);
    latxs_ra_free_temp(&temp_mxcsr);

#if defined(LATX_SYS_FCSR)
    latxs_append_ir2_opnd2i(LISA_ORI, &temp_int, zero, 1);
    latxs_append_ir2_opnd2i(LISA_ST_W, &temp_int, &latxs_env_ir2_opnd,
            offsetof(CPUX86State, is_fcsr_simd));

#if defined(FCSR_SAVE_RM_ONLY)
    latxs_append_ir2_opnd2i(LISA_ANDI, &fcsr_opnd, &fcsr_opnd, 0x300);
#endif
    latxs_append_ir2_opnd2i(LISA_ST_W, &fcsr_opnd,
            &latxs_env_ir2_opnd,
            offsetof(CPUX86State, fcsr));

    simd_fcsr_saved = 1;
#endif

    latxs_ra_free_temp(&temp_int);

    return fcsr_opnd;
}

#endif /* else LATX_SYS_NO_SIMD_FCSR */

void latxs_tr_reset_fcsr_rm(void)
{
    IR2_OPND *fcsr = &latxs_fcsr_ir2_opnd;
    IR2_OPND tmp = latxs_ra_alloc_itemp();

    latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, &tmp, fcsr);

    latxs_append_ir2_opnd2i(LISA_ORI,  &tmp, &tmp, 0x300);
    latxs_append_ir2_opnd2i(LISA_XORI, &tmp, &tmp, 0x300);

    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, fcsr, &tmp);
}

void latxs_update_fcsr(void *_env)
{
    CPUX86State *env = _env;
#if defined(LATX_SYS_FCSR)
    lsassertm(!env->is_fcsr_simd, "simd fcsr update\n");
    env->fcsr = env->fcsr & ~0x300;
    switch (env->fpuc & 0xc00) {
    default:
    case 0x000: /* x86 00 -> LA 00 */
        break;
    case 0x400: /* x86 01 -> LA 11 */
        env->fcsr |= 0x300;
        break;
    case 0x800: /* x86 10 -> LA 10 */
        env->fcsr |= 0x200;
        break;
    case 0xc00: /* x86 11 -> LA 01 */
        env->fcsr |= 0x100;
        break;
    }
#endif
    /* TODO: support fpu exception */
#if defined(LATX_SYS_FCSR_EXCP)
    /* exception enable */
    env->fcsr ^= (env->fpuc & (0x1 << 0)) << 4; /* x86 IM 0 -> LA 4 */
    env->fcsr ^= (env->fpuc & (0x1 << 1)) >> 1; /* x86 DM 1 -> LA 0 */
    env->fcsr ^= (env->fpuc & (0x1 << 2)) << 1; /* x86 ZM 2 -> LA 3 */
    env->fcsr ^= (env->fpuc & (0x1 << 3)) >> 1; /* x86 OM 3 -> LA 2 */
    env->fcsr ^= (env->fpuc & (0x1 << 4)) >> 3; /* x86 UM 4 -> LA 1 */

    /* statuc word flag */
    env->fcsr |= (env->fpus & (0x1 << 0)) << 20; /* x86 IE 0 -> LA 20 */
    env->fcsr |= (env->fpus & (0x1 << 2)) << 17; /* x86 ZE 2 -> LA 19 */
    env->fcsr |= (env->fpus & (0x1 << 3)) << 15; /* x86 OE 3 -> LA 18 */
    env->fcsr |= (env->fpus & (0x1 << 4)) << 13; /* x86 UE 4 -> LA 17 */
    env->fcsr |= (env->fpus & (0x1 << 5)) << 11; /* x86 UE 5 -> LA 16 */
#endif
}

void latxs_save_fcsr_cs_helper_prologue(IR2_OPND *tmp)
{
#ifndef LATX_SYS_FCSR
    lsassertm(0, "sys fcsr is used but undefined.");
#endif
    IR2_OPND *env = &latxs_env_ir2_opnd;
    IR2_OPND *fcsr = &latxs_fcsr_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;

    IR2_OPND fcsr_is_simd = latxs_ir2_opnd_new_label();
    IR2_OPND label_finish = latxs_ir2_opnd_new_label();

    latxs_append_ir2_opnd2i(LISA_LD_W, tmp, env,
            offsetof(CPUX86State, is_fcsr_simd));
    latxs_append_ir2_opnd3(LISA_BNE, tmp, zero, &fcsr_is_simd);

    /* LA FCSR => env->fcsr */
    latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, tmp, fcsr);
#if defined(FCSR_SAVE_RM_ONLY)
    latxs_append_ir2_opnd2i(LISA_ANDI, tmp, tmp, 0x300);
#endif
    latxs_append_ir2_opnd2i(LISA_ST_W, tmp, env,
            lsenv_offset_of_fcsr(lsenv));
    latxs_append_ir2_opnd1(LISA_B, &label_finish);

    latxs_append_ir2_opnd1(LISA_LABEL, &fcsr_is_simd);
    /* LA FCSR => env->fcsr_simd */
    latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, tmp, fcsr);
#if defined(FCSR_SAVE_RM_ONLY)
    latxs_append_ir2_opnd2i(LISA_ANDI, tmp, tmp, 0x300);
#endif
    latxs_append_ir2_opnd2i(LISA_ST_W, tmp, env,
            offsetof(CPUX86State, fcsr_simd));

    latxs_append_ir2_opnd1(LISA_LABEL, &label_finish);
}

void latxs_save_fcsr_scs_prologue(void)
{
#ifndef LATX_SYS_FCSR
    lsassertm(0, "sys fcsr is used but undefined.");
#endif
    IR2_OPND *env = &latxs_env_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;
    IR2_OPND *fcsr = &latxs_fcsr_ir2_opnd;
    IR2_OPND *stmp1 = &latxs_stmp1_ir2_opnd;

    IR2_OPND fcsr_is_simd = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd2i(LISA_LD_W, stmp1, env,
            offsetof(CPUX86State, is_fcsr_simd));
    latxs_append_ir2_opnd3(LISA_BNE, stmp1, zero, &fcsr_is_simd);

    /* LA FCSR => env->fcsr */
    latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, stmp1, fcsr);
#if defined(FCSR_SAVE_RM_ONLY)
    if(option_sigfpe) {
        latxs_append_ir2_opnd2i(LISA_ANDI, stmp1, stmp1, 0x31f);
    } else {
        latxs_append_ir2_opnd2i(LISA_ANDI, stmp1, stmp1, 0x300);
    }
#endif
    latxs_append_ir2_opnd2i(LISA_ST_W, stmp1, env,
            lsenv_offset_of_fcsr(lsenv));
    IR2_OPND label_next = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd1(LISA_B, &label_next);

    latxs_append_ir2_opnd1(LISA_LABEL, &fcsr_is_simd);
    /* LA FCSR => env->fcsr_is_simd */
    latxs_append_ir2_opnd2(LISA_MOVFCSR2GR, stmp1, fcsr);
#if defined(FCSR_SAVE_RM_ONLY)
    if(option_sigfpe) {
        latxs_append_ir2_opnd2i(LISA_ANDI, stmp1, stmp1, 0x31f);
    } else {
        latxs_append_ir2_opnd2i(LISA_ANDI, stmp1, stmp1, 0x300);
    }
#endif
    latxs_append_ir2_opnd2i(LISA_ST_W, stmp1, env,
            offsetof(CPUX86State, fcsr_simd));

    latxs_append_ir2_opnd1(LISA_LABEL, &label_next);
}

void latxs_load_dbt_fcsr(IR2_OPND *tmp)
{
#ifndef LATX_SYS_FCSR
    lsassertm(0, "sys fcsr is used but undefined.");
#endif
    IR2_OPND *fcsr = &latxs_fcsr_ir2_opnd;
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&latxs_sp_ir2_opnd);
    latxs_append_ir2_opnd2i(LISA_LD_D, tmp, &latxs_sp_ir2_opnd, 128);
    if (hamt_enabled()) latxs_tr_dmw_out_fix_mem(&latxs_sp_ir2_opnd);
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, fcsr, tmp);
}

void latxs_load_fcsr_cs_helper_epilogue(IR2_OPND *tmp)
{
#ifndef LATX_SYS_FCSR
    lsassertm(0, "sys fcsr is used but undefined.");
#endif
    IR2_OPND *env = &latxs_env_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;

    IR2_OPND fcsr_is_simd = latxs_ir2_opnd_new_label();
    IR2_OPND label_finish = latxs_ir2_opnd_new_label();

    latxs_append_ir2_opnd2i(LISA_LD_W, tmp, env,
            offsetof(CPUX86State, is_fcsr_simd));
    latxs_append_ir2_opnd3(LISA_BNE, tmp, zero, &fcsr_is_simd);

    /* LA FCSR <= env->fcsr */
    latxs_append_ir2_opnd2i(LISA_LD_W, tmp, env,
            offsetof(CPUX86State, fcsr));
    latxs_append_ir2_opnd1(LISA_B, &label_finish);

    latxs_append_ir2_opnd1(LISA_LABEL, &fcsr_is_simd);
    /* LA FCSR <= env->fcsr_simd */
    latxs_append_ir2_opnd2i(LISA_LD_W, tmp, env,
            offsetof(CPUX86State, fcsr_simd));

    latxs_append_ir2_opnd1(LISA_LABEL, &label_finish);
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, &latxs_fcsr_ir2_opnd, tmp);
}

void latxs_load_fcsr_scs_epilogue(void)
{
#ifndef LATX_SYS_FCSR
    lsassertm(0, "sys fcsr is used but undefined.");
#endif
    IR2_OPND *env = &latxs_env_ir2_opnd;
    IR2_OPND *zero = &latxs_zero_ir2_opnd;
    IR2_OPND *stmp1 = &latxs_stmp1_ir2_opnd;
    IR2_OPND *fcsr1 = &latxs_fcsr1_ir2_opnd; /* enable */
    IR2_OPND *fcsr2 = &latxs_fcsr2_ir2_opnd; /* Flags Cause */
    IR2_OPND *fcsr3 = &latxs_fcsr3_ir2_opnd; /* RM */

    /* load env->fcsr or env->fcsr_simd */
    IR2_OPND fcsr_is_simd = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd2i(LISA_LD_W, stmp1, env,
            offsetof(CPUX86State, is_fcsr_simd));
    latxs_append_ir2_opnd3(LISA_BNE, stmp1, zero, &fcsr_is_simd);

    /* LA FCSR <= env->fcsr */
    latxs_append_ir2_opnd2i(LISA_LD_W, stmp1, env,
            lsenv_offset_of_fcsr(lsenv));
    IR2_OPND label_finish = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd1(LISA_B, &label_finish);

    latxs_append_ir2_opnd1(LISA_LABEL, &fcsr_is_simd);
    /* LA FCSR <= env->fcsr_simd */
    latxs_append_ir2_opnd2i(LISA_LD_W, stmp1, env,
            offsetof(CPUX86State, fcsr_simd));

    latxs_append_ir2_opnd1(LISA_LABEL, &label_finish);
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, fcsr3, stmp1);
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, fcsr1, stmp1);
    latxs_append_ir2_opnd2(LISA_MOVGR2FCSR, fcsr2, zero);
}
