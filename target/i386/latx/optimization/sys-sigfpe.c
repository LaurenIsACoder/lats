/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <fenv.h>
#include <math.h>
#include <stddef.h>
#include "common.h"
#include "lsenv.h"
#include "latx-config.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "latx-sigfpe-sys.h"
#include "loongarch-extcontext.h"
#include "exec/exec-all.h"

#if defined(CONFIG_LATX_OPT) && !defined(CONFIG_LATX_TCG_MODE)
void latxs_save_sync_signal_state(CPUState *cpu, void *context)
{
    ucontext_t *uc = context;
    CPUX86State *env = cpu->env_ptr;
#ifdef LOONGARCH_NEW_WORLD
    struct extctx_layout extctx = { 0 };
#endif

    /* Only LATX translated code keeps guest registers in host registers. */
    if (!tcg_tb_lookup(uc->uc_mcontext.__pc)) {
        return;
    }

    /* Snapshot the mapped GPR and low XMM lanes before the TB is unwound. */
    for (unsigned r = 0; r < CPU_NB_REGS; r++) {
        unsigned host_reg = r < 8 ? r + 24 : r + 4;
        env->regs[r] = uc->uc_mcontext.__gregs[host_reg];
    }

#ifdef LOONGARCH_NEW_WORLD
    parse_extcontext(uc, &extctx);
    if (!UC_LSX(&extctx) && !UC_LASX(&extctx)) {
        return;
    }
#endif
    for (unsigned r = 0; r < CPU_NB_REGS; r++) {
        for (unsigned lane = 0; lane < 2; lane++) {
#ifdef LOONGARCH_NEW_WORLD
            uint64_t value = UC_GET_LSX(&extctx, r + 16, lane, uint64_t);
#else
            /* Oldworld reserves slot zero before the host FP registers. */
            const uint8_t *base = (const uint8_t *)&uc->uc_mcontext;
            size_t offset = offsetof(mcontext_t, __fpregs) +
                            (r + 17) * sizeof(uc->uc_mcontext.__fpregs[0]) +
                            lane * sizeof(uint64_t);
            uint64_t value;

            memcpy(&value, base + offset, sizeof(value));
#endif
            env->xmm_regs[r]._q_ZMMReg[lane] = value;
        }
    }
}
#endif

static int ieee754_to_int(float src) {
    if(isfinite(src)) {
        return (int)nearbyint(src);
    } else {
        return 0x80000000;
    }
}

static void latxs_rr_sigfpe_handler(int n, siginfo_t *siginfo, void *ctx)
{
    if ((!option_sigfpe)) return;
    if ((!option_fast_masked_v)) return;

    sigset_t new_set, old_set;

    sigemptyset(&new_set);
    sigaddset(&new_set, n);

    if (sigprocmask(SIG_BLOCK, &new_set, &old_set) != 0) {
        perror("sigprocmask_block");
        return;
    }

    ucontext_t *uctx = (ucontext_t *)ctx;
#ifdef LOONGARCH_NEW_WORLD
    struct extctx_layout extctx;
    memset(&extctx, 0, sizeof(extctx));
    parse_extcontext(uctx, &extctx);
#endif

    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    uint32_t inst, rd, vcause, vflag, mxcsr_mask, rj, rk;
#ifdef LOONGARCH_NEW_WORLD
    vcause = (UC_GET_FCSR(&extctx, int32_t) >> 28) & 0x1;
    vflag = (UC_GET_FCSR(&extctx, int32_t) >> 20) & 0x1;
#else
    vcause = (uctx->uc_mcontext.__fcsr >> 28) & 0x1;
    vflag  = (uctx->uc_mcontext.__fcsr >> 20) & 0x1;
#endif
    (void)vcause;
    (void)vflag;
    mxcsr_mask = (env->mxcsr >> 7) & 0x3f;
    inst = *(uint32_t *)uctx->uc_mcontext.__pc;
    rd = inst & 0x1f;
    rj = ((inst >> 5) & 0x1f);
    rk = ((inst >> 10) & 0x1f);

//    printf("In handler inst %x rd = %d, fcsr = %x, mxcsr = %x!\n", inst, rd, uctx->uc_mcontext.__fcsr, env->mxcsr);

//    In handler inst 713ad2b5 rd = 21, fcsr = 170010, mxcsr = 1fb9!

//    if ((vcause == 0) && (mxcsr_mask == 0x3f)) {
    if (mxcsr_mask == 0x3f) {
        switch(inst >> 10) {
        case 0x4512:    /* fsqrt.d */
#ifdef LOONGARCH_NEW_WORLD
            UC_SET_FPR(&extctx, rd, 0xfff8000000000000, uint64_t);
#else
            uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = 0xfff8000000000000;
#endif
            break;
        case 0x4511:    /* fsqrt.s */
#ifdef LOONGARCH_NEW_WORLD
            UC_SET_FPR(&extctx, rd, 0xffc00000, uint32_t);
#else
            uctx->uc_mcontext.__fpregs[rd+1].__val32[0] = 0xffc00000;
#endif
            break;
        case 0x46c1:    /* ftint.w.s */
        case 0x46c2:    /* ftint.w.d */
        case 0x46a1:    /* ftintrz.w.s */
        case 0x46a2:    /* ftintrz.w.d */
#ifdef LOONGARCH_NEW_WORLD
            UC_SET_FPR(&extctx, rd, 0x80000000, uint32_t);
#else
            uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = 0x80000000;
#endif
            break;
        case 0x46c9:    /* ftint.l.s */
        case 0x46ca:    /* ftint.l.d */
        case 0x46a9:    /* ftintrz.l.s */
        case 0x46aa:    /* ftintrz.l.d */
#ifdef LOONGARCH_NEW_WORLD
            UC_SET_FPR(&extctx, rd, 0x8000000000000000, uint64_t);
#else
            uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = 0x8000000000000000;
#endif
            break;
        case 0x1ca78c:    /* vftint.w.s */
            {
#ifdef LOONGARCH_NEW_WORLD
            uint32_t vj3 = UC_GET_LSX(&extctx, rj, 3, int32_t);
            float *src3 = (float *)&vj3;
            int result3 = ieee754_to_int(*src3);
            if (result3 == 0x7fffffff) result3 = 0x80000000;
            UC_SET_LSX(&extctx, rd, 3, result3, uint32_t);
            uint32_t vj2 = (float)UC_GET_LSX(&extctx, rj, 2, int32_t);
            float *src2 = (float *)&vj2;
            int result2 = ieee754_to_int(*src2);
            if (result2 == 0x7fffffff) result2 = 0x80000000;
            UC_SET_LSX(&extctx, rd, 2, result2, uint32_t);
            uint32_t vj1 = (float)UC_GET_LSX(&extctx, rj, 1, int32_t);
            float *src1 = (float *)&vj1;
            int result1 = ieee754_to_int(*src1);
            if (result1 == 0x7fffffff) result1 = 0x80000000;
            UC_SET_LSX(&extctx, rd, 1, result1, uint32_t);
            uint32_t vj0 = (float)UC_GET_LSX(&extctx, rj, 0, int32_t);
            float *src0 = (float *)&vj0;
            int result0 = ieee754_to_int(*src0);
            if (result0 == 0x7fffffff) result0 = 0x80000000;
            UC_SET_LSX(&extctx, rd, 0, result0, uint32_t);

            UC_SET_FCSR(&extctx, ((UC_GET_FCSR(&extctx, int32_t)) | 0x10000000), int32_t);
#else

            float *src3 = (float*)&uctx->uc_mcontext.__fpregs[rj+1].__val32[3];
            int result3 = ieee754_to_int(*src3);
            if (result3 == 0x7fffffff) result3 = 0x80000000;
            uctx->uc_mcontext.__fpregs[rd+1].__val32[3] = result3;
            float *src2 = (float*)&uctx->uc_mcontext.__fpregs[rj+1].__val32[2];
            int result2 = ieee754_to_int(*src2);
            if (result2 == 0x7fffffff) result2 = 0x80000000;
            uctx->uc_mcontext.__fpregs[rd+1].__val32[2] = result2;
            float *src1 = (float*)&uctx->uc_mcontext.__fpregs[rj+1].__val32[1];
            int result1 = ieee754_to_int(*src1);
            if (result1 == 0x7fffffff) result1 = 0x80000000;
            uctx->uc_mcontext.__fpregs[rd+1].__val32[1] = result1;
            float *src0 = (float*)&uctx->uc_mcontext.__fpregs[rj+1].__val32[0];
            int result0 = ieee754_to_int(*src0);
            if (result0 == 0x7fffffff) result0 = 0x80000000;
            uctx->uc_mcontext.__fpregs[rd+1].__val32[0] = result0;

            uctx->uc_mcontext.__fcsr |= 0x10000000;
#endif
            break;
            }
        case 0x1ca792:    /* vftintrz.w.s */
#ifdef LOONGARCH_NEW_WORLD
            UC_SET_FPR(&extctx, rd, 0x80000000, uint32_t);
#else
            uctx->uc_mcontext.__fpregs[rd+1].__val32[0] = 0x80000000;
#endif
            break;
        case 0x1ca739:    /* vfsqrt.s */
            {
#ifdef LOONGARCH_NEW_WORLD
                uint32_t rj1 = UC_GET_LSX(&extctx, rj, 3, int32_t);
                float *vj1 = (float *)&rj1;
                float result1 = sqrt(*vj1);
                unsigned int result_hex1 = *(unsigned int*)&result1;
                UC_SET_LSX(&extctx, rd, 3, result_hex1, uint32_t);
                uint32_t rj2 = UC_GET_LSX(&extctx, rj, 2, int32_t);
                float *vj2 = (float *)&rj2;
                float result2 = sqrt(*vj2);
                unsigned int result_hex2 = *(unsigned int*)&result2;
                UC_SET_LSX(&extctx, rd, 2, result_hex2, uint32_t);
                uint32_t rj3 = UC_GET_LSX(&extctx, rj, 1, int32_t);
                float *vj3 = (float *)&rj3;
                float result3 = sqrt(*vj3);
                unsigned int result_hex3 = *(unsigned int*)&result3;
                UC_SET_LSX(&extctx, rd, 1, result_hex3, uint32_t);
                uint32_t rj4 = UC_GET_LSX(&extctx, rj, 0, int32_t);
                float *vj4 = (float *)&rj4;
                float result4 = sqrt(*vj4);
                unsigned int result_hex4 = *(unsigned int*)&result4;
                UC_SET_LSX(&extctx, rd, 0, result_hex4, uint32_t);
#else
                float *rj1 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[3];
                float result1 = sqrt(*rj1);
                unsigned int result_hex1 = *(unsigned int*)&result1;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[3] = result_hex1;
                float *rj2 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[2];
                float result2 = sqrt(*rj2);
                unsigned int result_hex2 = *(unsigned int*)&result2;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[2] = result_hex2;
                float *rj3 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[1];
                float result3 = sqrt(*rj3);
                unsigned int result_hex3 = *(unsigned int*)&result3;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[1] = result_hex3;
                float *rj4 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[0];
                float result4 = sqrt(*rj4);
                unsigned int result_hex4 = *(unsigned int*)&result4;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[0] = result_hex4;
#endif
                break;
            }
        case 0x1ca73a:    /* vfsqrt.d */
            {
#ifdef LOONGARCH_NEW_WORLD
                uint64_t rj1 = UC_GET_LSX(&extctx, rj, 1, int64_t);
                double *vj1 = (double *)&rj1;
                double result1 = sqrt(*vj1);
                uint64_t result_hex1 = *(uint64_t*)&result1;
                UC_SET_LSX(&extctx, rd, 1, result_hex1, uint64_t);
                uint64_t rj2 = UC_GET_LSX(&extctx, rj, 0, int64_t);
                double *vj2 = (double *)&rj2;
                double result2 = sqrt(*vj2);
                uint64_t result_hex2 = *(uint64_t*)&result2;
                UC_SET_LSX(&extctx, rd, 0, result_hex2, uint64_t);
#else
                double *rj1 = (double *)&uctx->uc_mcontext.__fpregs[rj+1].__val64[1];
                double result1 = sqrt(*rj1);
                uint64_t result_hex1 = *(uint64_t*)&result1;
                uctx->uc_mcontext.__fpregs[rd+1].__val64[1] = result_hex1;
                double *rj2 = (double *)&uctx->uc_mcontext.__fpregs[rj+1].__val64[0];
                double result2 = sqrt(*rj2);
                uint64_t result_hex2 = *(uint64_t*)&result2;
                uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = result_hex2;
#endif
                break;
            }
        default:
            break;
        }
        switch(inst >> 15) {
        case 0xe293:    /* vftint.w.d */
#ifdef LOONGARCH_NEW_WORLD
            UC_SET_FPR(&extctx, rd, 0x80000000, uint32_t);
#else
            uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = 0x80000000;
#endif
            break;
        case 0xe296:    /* vftintrz.w.d */
#ifdef LOONGARCH_NEW_WORLD
            UC_SET_FPR(&extctx, rd, 0x80000000, uint32_t);
#else
            uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = 0x80000000;
#endif
            break;
        case 0xe261:    /* vfadd.s */
            {
#ifdef LOONGARCH_NEW_WORLD
                uint32_t rj1 = UC_GET_LSX(&extctx, rj, 3, int32_t);
                uint32_t rk1 = UC_GET_LSX(&extctx, rk, 3, int32_t);
                float *vj1 = (float *)&rj1;
                float *vk1 = (float *)&rk1;
                float result1 = *vj1 + *vk1;
                unsigned int result_hex1 = *(unsigned int*)&result1;
                UC_SET_LSX(&extctx, rd, 3, result_hex1, uint32_t);
                uint32_t rj2 = UC_GET_LSX(&extctx, rj, 2, int32_t);
                uint32_t rk2 = UC_GET_LSX(&extctx, rk, 2, int32_t);
                float *vj2 = (float *)&rj2;
                float *vk2 = (float *)&rk2;
                float result2 = *vj2 + *vk2;
                unsigned int result_hex2 = *(unsigned int*)&result2;
                UC_SET_LSX(&extctx, rd, 2, result_hex2, uint32_t);
                uint32_t rj3 = UC_GET_LSX(&extctx, rj, 1, int32_t);
                uint32_t rk3 = UC_GET_LSX(&extctx, rk, 1, int32_t);
                float *vj3 = (float *)&rj3;
                float *vk3 = (float *)&rk3;
                float result3 = *vj3 + *vk3;
                unsigned int result_hex3 = *(unsigned int*)&result3;
                UC_SET_LSX(&extctx, rd, 1, result_hex3, uint32_t);
                uint32_t rj4 = UC_GET_LSX(&extctx, rj, 0, int32_t);
                uint32_t rk4 = UC_GET_LSX(&extctx, rk, 0, int32_t);
                float *vj4 = (float *)&rj4;
                float *vk4 = (float *)&rk4;
                float result4 = *vj4 + *vk4;
                unsigned int result_hex4 = *(unsigned int*)&result4;
                UC_SET_LSX(&extctx, rd, 0, result_hex4, uint32_t);
#else
                float *rj1 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[3];
                float *rk1 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[3];
                float result1 = *rj1 + *rk1;
                unsigned int result_hex1 = *(unsigned int*)&result1;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[3] = result_hex1;
                float *rj2 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[2];
                float *rk2 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[2];
                float result2 = *rj2 + *rk2;
                unsigned int result_hex2 = *(unsigned int*)&result2;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[2] = result_hex2;
                float *rj3 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[1];
                float *rk3 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[1];
                float result3 = *rj3 + *rk3;
                unsigned int result_hex3 = *(unsigned int*)&result3;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[1] = result_hex3;
                float *rj4 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[0];
                float *rk4 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[0];
                float result4 = *rj4 + *rk4;
                unsigned int result_hex4 = *(unsigned int*)&result4;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[0] = result_hex4;
#endif
                break;
            }
        case 0xe265:    /* vfsub.s */
            {
#ifdef LOONGARCH_NEW_WORLD
                uint32_t rj1 = UC_GET_LSX(&extctx, rj, 3, int32_t);
                uint32_t rk1 = UC_GET_LSX(&extctx, rk, 3, int32_t);
                float *vj1 = (float *)&rj1;
                float *vk1 = (float *)&rk1;
                float result1 = *vj1 - *vk1;
                unsigned int result_hex1 = *(unsigned int*)&result1;
                UC_SET_LSX(&extctx, rd, 3, result_hex1, uint32_t);
                uint32_t rj2 = UC_GET_LSX(&extctx, rj, 2, int32_t);
                uint32_t rk2 = UC_GET_LSX(&extctx, rk, 2, int32_t);
                float *vj2 = (float *)&rj2;
                float *vk2 = (float *)&rk2;
                float result2 = *vj2 - *vk2;
                unsigned int result_hex2 = *(unsigned int*)&result2;
                UC_SET_LSX(&extctx, rd, 2, result_hex2, uint32_t);
                uint32_t rj3 = UC_GET_LSX(&extctx, rj, 1, int32_t);
                uint32_t rk3 = UC_GET_LSX(&extctx, rk, 1, int32_t);
                float *vj3 = (float *)&rj3;
                float *vk3 = (float *)&rk3;
                float result3 = *vj3 - *vk3;
                unsigned int result_hex3 = *(unsigned int*)&result3;
                UC_SET_LSX(&extctx, rd, 1, result_hex3, uint32_t);
                uint32_t rj4 = UC_GET_LSX(&extctx, rj, 0, int32_t);
                uint32_t rk4 = UC_GET_LSX(&extctx, rk, 0, int32_t);
                float *vj4 = (float *)&rj4;
                float *vk4 = (float *)&rk4;
                float result4 = *vj4 - *vk4;
                unsigned int result_hex4 = *(unsigned int*)&result4;
                UC_SET_LSX(&extctx, rd, 0, result_hex4, uint32_t);
#else
                float *rj1 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[3];
                float *rk1 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[3];
                float result1 = *rj1 - *rk1;
                unsigned int result_hex1 = *(unsigned int*)&result1;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[3] = result_hex1;
                float *rj2 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[2];
                float *rk2 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[2];
                float result2 = *rj2 - *rk2;
                unsigned int result_hex2 = *(unsigned int*)&result2;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[2] = result_hex2;
                float *rj3 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[1];
                float *rk3 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[1];
                float result3 = *rj3 - *rk3;
                unsigned int result_hex3 = *(unsigned int*)&result3;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[1] = result_hex3;
                float *rj4 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[0];
                float *rk4 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[0];
                float result4 = *rj4 - *rk4;
                unsigned int result_hex4 = *(unsigned int*)&result4;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[0] = result_hex4;
#endif
                break;
            }
        case 0xe271:    /* vfmul.s */
            {
#ifdef LOONGARCH_NEW_WORLD
                uint32_t rj1 = UC_GET_LSX(&extctx, rj, 3, int32_t);
                uint32_t rk1 = UC_GET_LSX(&extctx, rk, 3, int32_t);
                float *vj1 = (float *)&rj1;
                float *vk1 = (float *)&rk1;
                float result1 = (*vj1) * (*vk1);
                unsigned int result_hex1 = *(unsigned int*)&result1;
                UC_SET_LSX(&extctx, rd, 3, result_hex1, uint32_t);
                uint32_t rj2 = UC_GET_LSX(&extctx, rj, 2, int32_t);
                uint32_t rk2 = UC_GET_LSX(&extctx, rk, 2, int32_t);
                float *vj2 = (float *)&rj2;
                float *vk2 = (float *)&rk2;
                float result2 = (*vj2) * (*vk2);
                unsigned int result_hex2 = *(unsigned int*)&result2;
                UC_SET_LSX(&extctx, rd, 2, result_hex2, uint32_t);
                uint32_t rj3 = UC_GET_LSX(&extctx, rj, 1, int32_t);
                uint32_t rk3 = UC_GET_LSX(&extctx, rk, 1, int32_t);
                float *vj3 = (float *)&rj3;
                float *vk3 = (float *)&rk3;
                float result3 = (*vj3) * (*vk3);
                unsigned int result_hex3 = *(unsigned int*)&result3;
                UC_SET_LSX(&extctx, rd, 1, result_hex3, uint32_t);
                uint32_t rj4 = UC_GET_LSX(&extctx, rj, 0, int32_t);
                uint32_t rk4 = UC_GET_LSX(&extctx, rk, 0, int32_t);
                float *vj4 = (float *)&rj4;
                float *vk4 = (float *)&rk4;
                float result4 = (*vj4) * (*vk4);
                unsigned int result_hex4 = *(unsigned int*)&result4;
                UC_SET_LSX(&extctx, rd, 0, result_hex4, uint32_t);
#else
                float *rj1 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[3];
                float *rk1 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[3];
                float result1 = (*rj1) * (*rk1);
                unsigned int result_hex1 = *(unsigned int*)&result1;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[3] = result_hex1;
                float *rj2 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[2];
                float *rk2 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[2];
                float result2 = (*rj2) * (*rk2);
                unsigned int result_hex2 = *(unsigned int*)&result2;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[2] = result_hex2;
                float *rj3 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[1];
                float *rk3 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[1];
                float result3 = (*rj3) * (*rk3);
                unsigned int result_hex3 = *(unsigned int*)&result3;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[1] = result_hex3;
                float *rj4 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[0];
                float *rk4 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[0];
                float result4 = (*rj4) * (*rk4);
                unsigned int result_hex4 = *(unsigned int*)&result4;
                uctx->uc_mcontext.__fpregs[rd+1].__val32[0] = result_hex4;
#endif
                break;
            }
        case 0xe275:    /* vfdiv.s */
            {
#ifdef LOONGARCH_NEW_WORLD
                uint32_t rj1 = UC_GET_LSX(&extctx, rj, 3, int32_t);
                uint32_t rk1 = UC_GET_LSX(&extctx, rk, 3, int32_t);
                float *vj1 = (float *)&rj1;
                float *vk1 = (float *)&rk1;
                float result1 = *vj1 / *vk1;
                unsigned int result_hex1 = *(unsigned int*)&result1;
                UC_SET_LSX(&extctx, rd, 3, result_hex1, uint32_t);
                uint32_t rj2 = UC_GET_LSX(&extctx, rj, 2, int32_t);
                uint32_t rk2 = UC_GET_LSX(&extctx, rk, 2, int32_t);
                float *vj2 = (float *)&rj2;
                float *vk2 = (float *)&rk2;
                float result2 = *vj2 / *vk2;
                unsigned int result_hex2 = *(unsigned int*)&result2;
                UC_SET_LSX(&extctx, rd, 2, result_hex2, uint32_t);
                uint32_t rj3 = UC_GET_LSX(&extctx, rj, 1, int32_t);
                uint32_t rk3 = UC_GET_LSX(&extctx, rk, 1, int32_t);
                float *vj3 = (float *)&rj3;
                float *vk3 = (float *)&rk3;
                float result3 = *vj3 / *vk3;
                unsigned int result_hex3 = *(unsigned int*)&result3;
                UC_SET_LSX(&extctx, rd, 1, result_hex3, uint32_t);
                uint32_t rj4 = UC_GET_LSX(&extctx, rj, 0, int32_t);
                uint32_t rk4 = UC_GET_LSX(&extctx, rk, 0, int32_t);
                float *vj4 = (float *)&rj4;
                float *vk4 = (float *)&rk4;
                float result4 = *vj4 / *vk4;
                unsigned int result_hex4 = *(unsigned int*)&result4;
                UC_SET_LSX(&extctx, rd, 0, result_hex4, uint32_t);
#else
                if((uctx->uc_mcontext.__fpregs[rj+1].__val32[3] == 0x0) && (uctx->uc_mcontext.__fpregs[rk+1].__val32[3] == 0x0)) {
                    uctx->uc_mcontext.__fpregs[rd+1].__val32[3] = 0xffc00000;
                } else {
                    float *rj1 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[3];
                    float *rk1 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[3];
                    float result = *rj1 / *rk1;
                    unsigned int result_hex = *(unsigned int*)&result;
                    uctx->uc_mcontext.__fpregs[rd+1].__val32[3] = result_hex;
                }
                if((uctx->uc_mcontext.__fpregs[rj+1].__val32[2] == 0x0) && (uctx->uc_mcontext.__fpregs[rk+1].__val32[2] == 0x0)) {
                    uctx->uc_mcontext.__fpregs[rd+1].__val32[2] = 0xffc00000;
                } else {
                    float *rj1 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[2];
                    float *rk1 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[2];
                    float result = *rj1 / *rk1;
                    unsigned int result_hex = *(unsigned int*)&result;
                    uctx->uc_mcontext.__fpregs[rd+1].__val32[2] = result_hex;
                }
                if((uctx->uc_mcontext.__fpregs[rj+1].__val32[1] == 0x0) && (uctx->uc_mcontext.__fpregs[rk+1].__val32[1] == 0x0)) {
                    uctx->uc_mcontext.__fpregs[rd+1].__val32[1] = 0xffc00000;
                } else {
                    float *rj1 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[1];
                    float *rk1 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[1];
                    float result = *rj1 / *rk1;
                    unsigned int result_hex = *(unsigned int*)&result;
                    uctx->uc_mcontext.__fpregs[rd+1].__val32[1] = result_hex;
                }
                if((uctx->uc_mcontext.__fpregs[rj+1].__val32[0] == 0x0) && (uctx->uc_mcontext.__fpregs[rk+1].__val32[0] == 0x0)) {
                    uctx->uc_mcontext.__fpregs[rd+1].__val32[0] = 0xffc00000;
                } else {
                    float *rj1 = (float *)&uctx->uc_mcontext.__fpregs[rj+1].__val32[0];
                    float *rk1 = (float *)&uctx->uc_mcontext.__fpregs[rk+1].__val32[0];
                    float result = *rj1 / *rk1;
                    unsigned int result_hex = *(unsigned int*)&result;
                    uctx->uc_mcontext.__fpregs[rd+1].__val32[0] = result_hex;
                }
#endif
                break;
            }
        case 0xe262:    /* vfadd.d */
            {
#ifdef LOONGARCH_NEW_WORLD
                uint64_t rj1 = UC_GET_LSX(&extctx, rj, 1, int64_t);
                uint64_t rk1 = UC_GET_LSX(&extctx, rk, 1, int64_t);
                double *vj1 = (double *)&rj1;
                double *vk1 = (double *)&rk1;
                double result1 = *vj1 + *vk1;
                uint64_t result_hex1 = *(uint64_t*)&result1;
                UC_SET_LSX(&extctx, rd, 1, result_hex1, uint64_t);
                uint64_t rj2 = UC_GET_LSX(&extctx, rj, 0, int64_t);
                uint64_t rk2 = UC_GET_LSX(&extctx, rk, 0, int64_t);
                double *vj2 = (double *)&rj2;
                double *vk2 = (double *)&rk2;
                double result2 = *vj2 + *vk2;
                uint64_t result_hex2 = *(uint64_t*)&result2;
                UC_SET_LSX(&extctx, rd, 0, result_hex2, uint64_t);
#else
                double *rj1 = (double *)&uctx->uc_mcontext.__fpregs[rj+1].__val64[1];
                double *rk1 = (double *)&uctx->uc_mcontext.__fpregs[rk+1].__val64[1];
                double result1 = *rj1 + *rk1;
                uint64_t result_hex1 = *(uint64_t*)&result1;
                uctx->uc_mcontext.__fpregs[rd+1].__val64[1] = result_hex1;
                double *rj2 = (double *)&uctx->uc_mcontext.__fpregs[rj+1].__val64[1];
                double *rk2 = (double *)&uctx->uc_mcontext.__fpregs[rk+1].__val64[1];
                double result2 = *rj2 + *rk2;
                uint64_t result_hex2 = *(uint64_t*)&result2;
                uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = result_hex2;
#endif
                break;
            }
        case 0xe266:    /* vfsub.d */
            {
#ifdef LOONGARCH_NEW_WORLD
                uint64_t rj1 = UC_GET_LSX(&extctx, rj, 1, int64_t);
                uint64_t rk1 = UC_GET_LSX(&extctx, rk, 1, int64_t);
                double *vj1 = (double *)&rj1;
                double *vk1 = (double *)&rk1;
                double result1 = *vj1 - *vk1;
                uint64_t result_hex1 = *(uint64_t*)&result1;
                UC_SET_LSX(&extctx, rd, 1, result_hex1, uint64_t);
                uint64_t rj2 = UC_GET_LSX(&extctx, rj, 0, int64_t);
                uint64_t rk2 = UC_GET_LSX(&extctx, rk, 0, int64_t);
                double *vj2 = (double *)&rj2;
                double *vk2 = (double *)&rk2;
                double result2 = *vj2 - *vk2;
                uint64_t result_hex2 = *(uint64_t*)&result2;
                UC_SET_LSX(&extctx, rd, 0, result_hex2, uint64_t);
#else
                double *rj1 = (double *)&uctx->uc_mcontext.__fpregs[rj+1].__val64[1];
                double *rk1 = (double *)&uctx->uc_mcontext.__fpregs[rk+1].__val64[1];
                double result1 = *rj1 - *rk1;
                uint64_t result_hex1 = *(uint64_t*)&result1;
                uctx->uc_mcontext.__fpregs[rd+1].__val64[1] = result_hex1;
                double *rj2 = (double *)&uctx->uc_mcontext.__fpregs[rj+1].__val64[1];
                double *rk2 = (double *)&uctx->uc_mcontext.__fpregs[rk+1].__val64[1];
                double result2 = *rj2 - *rk2;
                uint64_t result_hex2 = *(uint64_t*)&result2;
                uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = result_hex2;
#endif
                break;
            }
        case 0xe272:    /* vfmul.d */
            {
#ifdef LOONGARCH_NEW_WORLD
                uint64_t rj1 = UC_GET_LSX(&extctx, rj, 1, int64_t);
                uint64_t rk1 = UC_GET_LSX(&extctx, rk, 1, int64_t);
                double *vj1 = (double *)&rj1;
                double *vk1 = (double *)&rk1;
                double result1 = (*vj1) * (*vk1);
                uint64_t result_hex1 = *(uint64_t*)&result1;
                UC_SET_LSX(&extctx, rd, 1, result_hex1, uint64_t);
                uint64_t rj2 = UC_GET_LSX(&extctx, rj, 0, int64_t);
                uint64_t rk2 = UC_GET_LSX(&extctx, rk, 0, int64_t);
                double *vj2 = (double *)&rj2;
                double *vk2 = (double *)&rk2;
                double result2 = (*vj2) * (*vk2);
                uint64_t result_hex2 = *(uint64_t*)&result2;
                UC_SET_LSX(&extctx, rd, 0, result_hex2, uint64_t);
#else
                double *rj1 = (double *)&uctx->uc_mcontext.__fpregs[rj+1].__val64[1];
                double *rk1 = (double *)&uctx->uc_mcontext.__fpregs[rk+1].__val64[1];
                double result1 = (*rj1) * (*rk1);
                uint64_t result_hex1 = *(uint64_t*)&result1;
                uctx->uc_mcontext.__fpregs[rd+1].__val64[1] = result_hex1;
                double *rj2 = (double *)&uctx->uc_mcontext.__fpregs[rj+1].__val64[1];
                double *rk2 = (double *)&uctx->uc_mcontext.__fpregs[rk+1].__val64[1];
                double result2 = (*rj2) * (*rk2);
                uint64_t result_hex2 = *(uint64_t*)&result2;
                uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = result_hex2;
#endif
                break;
            }
        case 0xe276:    /* vfdiv.d */
            {
#ifdef LOONGARCH_NEW_WORLD
                uint64_t rj1 = UC_GET_LSX(&extctx, rj, 1, int64_t);
                uint64_t rk1 = UC_GET_LSX(&extctx, rk, 1, int64_t);
                double *vj1 = (double *)&rj1;
                double *vk1 = (double *)&rk1;
                double result1 = *vj1 / *vk1;
                uint64_t result_hex1 = *(uint64_t*)&result1;
                UC_SET_LSX(&extctx, rd, 1, result_hex1, uint64_t);
                uint64_t rj2 = UC_GET_LSX(&extctx, rj, 0, int64_t);
                uint64_t rk2 = UC_GET_LSX(&extctx, rk, 0, int64_t);
                double *vj2 = (double *)&rj2;
                double *vk2 = (double *)&rk2;
                double result2 = *vj2 / *vk2;
                uint64_t result_hex2 = *(uint64_t*)&result2;
                UC_SET_LSX(&extctx, rd, 0, result_hex2, uint64_t);
#else
                if((uctx->uc_mcontext.__fpregs[rj+1].__val64[1] == 0x0) && (uctx->uc_mcontext.__fpregs[rk+1].__val64[1] == 0x0)) {
                    uctx->uc_mcontext.__fpregs[rd+1].__val64[1] = 0xfff8000000000000;
                } else {
                    double *rj1 = (double *)&uctx->uc_mcontext.__fpregs[rj+1].__val64[1];
                    double *rk1 = (double *)&uctx->uc_mcontext.__fpregs[rk+1].__val64[1];
                    double result = *rj1 / *rk1;
                    uint64_t result_hex = *(uint64_t*)&result;
                    uctx->uc_mcontext.__fpregs[rd+1].__val64[1] = result_hex;
                }
                if((uctx->uc_mcontext.__fpregs[rj+1].__val64[0] == 0x0) && (uctx->uc_mcontext.__fpregs[rk+1].__val64[0] == 0x0)) {
                    uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = 0xfff8000000000000;
                } else {
                    double *rj1 = (double *)&uctx->uc_mcontext.__fpregs[rj+1].__val64[1];
                    double *rk1 = (double *)&uctx->uc_mcontext.__fpregs[rk+1].__val64[1];
                    double result = *rj1 / *rk1;
                    uint64_t result_hex = *(uint64_t*)&result;
                    uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = result_hex;
                }
#endif
                break;
            }
        case 0x201:    /* fadd.s */
        case 0x205:    /* fsub.s */
        case 0x209:    /* fmul.s */
        case 0x20d:    /* fdiv.s */
#ifdef LOONGARCH_NEW_WORLD
            UC_SET_FPR(&extctx, rd, 0xffc00000, uint32_t);
#else
            uctx->uc_mcontext.__fpregs[rd+1].__val32[0] = 0xffc00000;
#endif
            break;
        case 0x202:    /* fadd.d */
        case 0x206:    /* fsub.d */
        case 0x20a:    /* fmul.d */
        case 0x20e:    /* fdiv.d */
#ifdef LOONGARCH_NEW_WORLD
            UC_SET_FPR(&extctx, rd, 0xfff8000000000000, uint64_t);
#else
            uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = 0xfff8000000000000;
#endif
            break;
        default:
            break;
        }
        switch(inst >> 20) {
        case 0xc1:    /* fcmp.cond.S */
        case 0xc2:    /* fcmp.cond.D */
#ifdef LOONGARCH_NEW_WORLD
            UC_SET_FCC(&extctx, 0x1);
#else
            uctx->uc_mcontext.__fcc = 0x1;
#endif
            break;
        case 0xc5:    /* vfcmp.cond.S */
        case 0xc6:    /* vfcmp.cond.D */
#ifdef LOONGARCH_NEW_WORLD
            UC_SET_LSX(&extctx, rd, 1, 0xffffffffffffffff, uint64_t);
            UC_SET_LSX(&extctx, rd, 0, 0xffffffffffffffff, uint64_t);
#else
            uctx->uc_mcontext.__fpregs[rd+1].__val64[1] = 0xffffffffffffffff;
            uctx->uc_mcontext.__fpregs[rd+1].__val64[0] = 0xffffffffffffffff;
#endif
            break;
        default:
            break;
        }

#ifdef LOONGARCH_NEW_WORLD
        UC_SET_FCSR(&extctx, ((UC_GET_FCSR(&extctx, int32_t)) | 0x100000), int32_t);
        UC_SET_FCSR(&extctx, ((UC_GET_FCSR(&extctx, int32_t)) & 0x1f1f031f), int32_t);
#else
        uctx->uc_mcontext.__fcsr |= 0x100000;
        uctx->uc_mcontext.__fcsr &= 0x1f1f031f;
#endif
    } else if (mxcsr_mask == 0x3e) {
#ifdef LOONGARCH_NEW_WORLD
        UC_SET_FCSR(&extctx, ((UC_GET_FCSR(&extctx, int32_t)) | 0x100000), int32_t);
#else
        uctx->uc_mcontext.__fcsr |= 0x100000;
#endif
        env->excp_divz = 1;
    } else {
#ifdef LOONGARCH_NEW_WORLD
        UC_SET_FCSR(&extctx, ((UC_GET_FCSR(&extctx, int32_t)) & 0xffffff), int32_t);
#else
        uctx->uc_mcontext.__fcsr &= 0xffffff;
#endif
        env->excp_divz = 1;
    }

    uctx->uc_mcontext.__pc += 0x4;

    if(option_lazy_mxcsr) {
#ifdef LOONGARCH_NEW_WORLD
        uint32_t fcsr = UC_GET_FCSR(&extctx, uint32_t) >> 16;
#else
        uint32_t fcsr = uctx->uc_mcontext.__fcsr >> 16;
#endif
        if(fcsr & 0x1) {    /* PE */
            env->mxcsr |= 0x20;
        }
        if(fcsr & 0x2) {    /* UE */
            env->mxcsr |= 0x10;
        }
        if(fcsr & 0x4) {    /* OE */
            env->mxcsr |= 0x8;
        }
        if(fcsr & 0x8) {    /* ZE */
            env->mxcsr |= 0x4;
        }
        if(fcsr & 0x10) {    /* IE */
            env->mxcsr |= 0x1;
        }
    }

    if (sigprocmask(SIG_SETMASK, &old_set, NULL) != 0) {
        perror("sigprocmask_setmask");
        return;
    }

    return;
}

void latxs_init_rr_thread_sigfpe(CPUState *cpu)
{
    if (!option_sigfpe) {
        return;
    }
#if defined(CONFIG_LATX_TCG_MODE)
    return;
#endif

    /* 1. unblock the signal for vCPU thread */
    sigset_t set, oldset;
    sigemptyset(&set);
    sigaddset(&set, SIGFPE);

    int ret = pthread_sigmask(SIG_UNBLOCK, &set, &oldset);
    if (ret < 0) {
        fprintf(stderr, "[SIGFPE] unblock SIGFPE failed\n");
        exit(-1);
    }

    /* 2. set the handler for signal */

    struct sigaction act;
    struct sigaction old_act;

    memset(&act, 0, sizeof(act));
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = latxs_rr_sigfpe_handler;

    ret = sigaction(SIGFPE, &act, &old_act);
    if (ret < 0) {
        fprintf(stderr, "[SIGFPE] set handler failed\n");
        exit(-1);
    }

#ifdef CONFIG_LATX_PRINT
    pthread_t tid = pthread_self();
    fprintf(stderr, "[SIGFPE] thread %x set signal SIGFPE handler\n",
            (unsigned int)tid);
#endif

    if(option_fast_masked_v) {
        // enable SIGFPE V exception
        feenableexcept(FE_INVALID);
    }
}
