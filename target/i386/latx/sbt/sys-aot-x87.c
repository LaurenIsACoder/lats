/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "accel/tcg/tb-context.h"
#include "exec/helper-proto.h"
#include "tcg/helper-tcg.h"
#include "latx-helper.h"
#include "latx-options.h"
#include "lats/aot-x87.h"

void latx_helper_aot_x87(CPUX86State *env, unsigned descriptor,
                         uint64_t operand)
{
    unsigned simd = env->is_fcsr_simd;
    unsigned kind = LATS_X87_KIND(descriptor), reg = LATS_X87_REGISTER(descriptor);
    bool writes = kind == LATS_X87_FSTP_EXT || kind == LATS_X87_FNSTCW_MEM ||
                  kind == LATS_X87_FBSTP || kind == LATS_X87_FIST_STORE ||
                  kind == LATS_X87_FISTTP_STORE;
    unsigned mtcc = curr_mtcc(env);
    unsigned invalidations = qatomic_read(&tb_ctx.tb_phys_invalidate_count);
    unsigned flushes = qatomic_read(&tb_ctx.tb_flush_count);
    unsigned partial_flushes = qatomic_read(&tb_ctx.tb_flush_count_mtcc[mtcc]);

    g_assert(option_soft_fpu);
    env->native_recovered = true;
    env->is_fcsr_simd = 0;
    if (kind == LATS_X87_FWAIT) {
        if ((env->hflags & (HF_MP_MASK | HF_TS_MASK)) ==
            (HF_MP_MASK | HF_TS_MASK)) {
            raise_exception(env, EXCP07_PREX);
        }
    } else if (env->hflags & (HF_TS_MASK | HF_EM_MASK)) {
        raise_exception(env, EXCP07_PREX);
    }
    switch (kind) {
    case LATS_X87_FLD_EXT:
        helper_fldt_ST0(env, operand);
        break;
    case LATS_X87_FSTP_EXT:
        helper_fstt_ST0(env, operand);
        helper_fpop(env);
        break;
    case LATS_X87_FLD_REG:
        helper_fpush(env);
        helper_fmov_ST0_STN(env, (reg + 1) & 7);
        break;
    case LATS_X87_FSTP_REG:
        helper_fmov_STN_ST0(env, reg);
        helper_fpop(env);
        break;
    case LATS_X87_FABS:
        helper_fabs_ST0(env);
        break;
    case LATS_X87_FXCH:
        helper_fxchg_ST0_STN(env, reg);
        break;
    case LATS_X87_FXAM:
        helper_fxam_ST0(env);
        break;
    case LATS_X87_FCOMI:
    case LATS_X87_FCOMIP:
    case LATS_X87_FUCOMI:
    case LATS_X87_FUCOMIP:
        env->cc_src &= ~(CC_O | CC_S | CC_A);
        env->cc_op = CC_OP_EFLAGS;
#ifdef CONFIG_LATX_OPT_EFLAGS
        __lbt_mtflag_all(env->cc_src);
#endif
        helper_fmov_FT0_STN(env, reg);
        if (kind == LATS_X87_FCOMI || kind == LATS_X87_FCOMIP) {
            helper_fcomi_ST0_FT0(env);
        } else {
            helper_fucomi_ST0_FT0(env);
        }
        if (kind == LATS_X87_FCOMIP || kind == LATS_X87_FUCOMIP) {
            helper_fpop(env);
        }
        break;
    case LATS_X87_FNSTSW_AX:
        env->regs[R_EAX] = (env->regs[R_EAX] & ~UINT64_C(0xffff)) | helper_fnstsw(env);
        break;
    case LATS_X87_FNSTCW_MEM:
        cpu_stw_data(env, operand, helper_fnstcw(env));
        break;
    case LATS_X87_FPREM:
        helper_fprem(env);
        break;
    case LATS_X87_FPREM1:
        helper_fprem1(env);
        break;
    case LATS_X87_FXTRACT:
        helper_fxtract(env);
        break;
    case LATS_X87_FSIN:
        helper_fsin(env);
        break;
    case LATS_X87_FCOS:
        helper_fcos(env);
        break;
    case LATS_X87_FPATAN:
        helper_fpatan(env);
        break;
    case LATS_X87_FSINCOS:
        helper_fsincos(env);
        break;
    case LATS_X87_FPTAN:
        helper_fptan(env);
        break;
    case LATS_X87_FYL2X:
        helper_fyl2x(env);
        break;
    case LATS_X87_FYL2XP1:
        helper_fyl2xp1(env);
        break;
    case LATS_X87_F2XM1:
        helper_f2xm1(env);
        break;
    case LATS_X87_FBLD:
        helper_fbld_ST0(env, operand);
        break;
    case LATS_X87_FBSTP:
        helper_fbst_ST0(env, operand);
        helper_fpop(env);
        break;
    case LATS_X87_FILD_I64:
        helper_fildll_ST0(env, operand);
        break;
    case LATS_X87_FIST_STORE:
    case LATS_X87_FISTTP_STORE: {
        bool truncate = kind == LATS_X87_FISTTP_STORE;
        unsigned width = reg & 3;
        uint64_t value =
            width == 0
                ? (truncate ? helper_fistt_ST0(env) : helper_fist_ST0(env))
            : width == 1
                ? (truncate ? helper_fisttl_ST0(env) : helper_fistl_ST0(env))
                : (truncate ? helper_fisttll_ST0(env) : helper_fistll_ST0(env));
        if (width == 0) {
            cpu_stw_data(env, operand, value);
        } else if (width == 1) {
            cpu_stl_data(env, operand, value);
        } else {
            cpu_stq_data(env, operand, value);
        }
        if (truncate || (reg & 4)) {
            helper_fpop(env);
        }
        break;
    }
    case LATS_X87_FWAIT:
    case LATS_X87_FNOP:
        helper_fwait(env);
        break;
    default:
        g_assert_not_reached();
    }
    /* A C helper's return address cannot identify the current native TB to
     * the store slow path. Resume from committed state after invalidation. */
    if ((writes &&
         invalidations != qatomic_read(&tb_ctx.tb_phys_invalidate_count)) ||
        flushes != qatomic_read(&tb_ctx.tb_flush_count) ||
        partial_flushes != qatomic_read(&tb_ctx.tb_flush_count_mtcc[mtcc]) ||
        qatomic_read(&env->ras_tp.reset_pending)) {
        env->eip += LATS_X87_LENGTH(descriptor);
        cpu_loop_exit_noexc(env_cpu(env));
    }
    env->is_fcsr_simd = simd;
    env->native_recovered = false;
}
