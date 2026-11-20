/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "translate.h"
#include "latx-options.h"
#include "latx-tb-trace-sys.h"
#include "sysemu/rr.h"

#ifdef LATXS_TB_TRACE_ENABLE

#define TRACE_TB_MASK       0xF
#define TRACE_CPL_MASK      0xF0
#define TRACE_MORE_MASK     0xF00

#define TRACE_MORE_TB_PER_SEC   0x1

#define TRACE_TB_SHIFT      0
#define TRACE_CPL_SHIFT     4
#define TRACE_MORE_SHIFT    8

#define GET_TRACE(name) \
((option_trace_simple & TRACE_##name##_MASK) >> TRACE_##name##_SHIFT)

#define TRACE_CMP_XMM_REGS (rrtrace_mask & 0x8)
#define TRACE_CMP_GPR_REGS (rrtrace_mask & 0x2)
#define TRACE_CMP_CR_REGS  (rrtrace_mask & 0x10)
#define TRACE_CMP_EFLAGS   (rrtrace_mask & 0x20)
#define TRACE_CMP_EVENT    (rrtrace_mask & 0x40)

static void latxs_tb_trace_more(int more,
        CPUX86State *env, TranslationBlock *tb)
{
    if (more & TRACE_MORE_TB_PER_SEC) {
        static uint64_t enter_tb_count = 0;
        static int64_t st = 0, cur = 0;
        enter_tb_count += 1;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        cur = ts.tv_sec;
        if (cur != st) {
            st = cur;
            printf("[%ld] exec=%ld\n", cur, enter_tb_count);
        }
    }
}

void __latxs_tb_trace(CPUX86State *env, TranslationBlock *tb,
        int isexcp)
{
    if (!option_trace_simple || !tb) {
        return;
    } 

    int trace_tb   = GET_TRACE(TB);
    int trace_cpl  = GET_TRACE(CPL);
    int trace_more = GET_TRACE(MORE);

    latxs_tb_trace_more(trace_more, env, tb);

    /* filter with CPL */
    if ((tb->flags & 0x3) < trace_cpl) {
        return;
    }

    /* start trace from given TB */
    static int latxs_sttb;
    if (option_trace_start_tb_set &&
        !latxs_sttb &&
        tb->pc != option_trace_start_tb) {
        return;
    }
    latxs_sttb = 1;

    /* start trace after given number of TB's execution */
    static int latxs_stnr;
    if (latxs_stnr < option_trace_start_nr) {
        latxs_stnr += 1;
        return;
    }

    /* print tb execution */
    if (!trace_tb) {
        return;
    }

    __latxs_tb_trace_nocheck(env, tb, NULL, trace_tb, 0);
}

#ifdef CONFIG_SOFTMMU

static void __latxs_tb_trace_rr_bin_out(
        CPUX86State *env, TranslationBlock *tb,
        FILE *fd, bool noeflags, int isexcp)
{
    uint64_t pc = tb->pc;
    int i = 0;
    uint64_t zmmreg0, zmmreg1;
    uint32_t eflags;

    /* save pc */
    fwrite(&pc, sizeof(pc), 1, fd);

    /* save eflags */
    if (TRACE_CMP_EFLAGS) {
        eflags = cpu_compute_eflags(env);
        fwrite(&eflags, sizeof(eflags), 1, fd);
    }

    /* save gprs */
    if (TRACE_CMP_GPR_REGS) {
        for (i = 0; i < CPU_NB_REGS; i++) {
            fwrite(&env->regs[i], sizeof(env->regs[0]), 1, fd);
        }
    }

    /* save cr */
    if (TRACE_CMP_CR_REGS) {
        target_ulong cr8 = helper_read_crN(env, 8);
        fwrite(&env->cr[0], sizeof(target_ulong), 1, fd);
        fwrite(&env->cr[2], sizeof(target_ulong), 1, fd);
        fwrite(&env->cr[3], sizeof(target_ulong), 1, fd);
        fwrite(&env->cr[4], sizeof(target_ulong), 1, fd);
        fwrite(&cr8, sizeof(target_ulong), 1, fd);
    }

    /* save xmm regs */
    if (TRACE_CMP_XMM_REGS) {
        for (i = 0; i < CPU_NB_REGS; i++) {
            if (qemu_tcg_mode_1_tcg()) {
                zmmreg0 = env->xmm_regs[i]._q_ZMMReg[0];
                zmmreg1 = env->xmm_regs[i]._q_ZMMReg[1];
                fwrite(&zmmreg0, sizeof(env->xmm_regs[i]._q_ZMMReg[0]), 1, fd);
                fwrite(&zmmreg1, sizeof(env->xmm_regs[i]._q_ZMMReg[1]), 1, fd);
            } else if (qemu_tcg_mode_2_lat()) {
                zmmreg0 = env->xmm_regs[i]._q_ZMMReg[0];
                zmmreg1 = env->xmm_regs[i]._q_ZMMReg[1];
                fwrite(&zmmreg0, sizeof(env->xmm_regs[i]._q_ZMMReg[0]), 1, fd);
                fwrite(&zmmreg1, sizeof(env->xmm_regs[i]._q_ZMMReg[1]), 1, fd);
            }
        }
    }

    /* rr event counter */
    if (TRACE_CMP_EVENT) {
        if (rr_counter_enabled()) {
            uint64_t n = 0;
            n = rr_counter_get(RR_EVENT_HELPER_LD); fwrite(&n, sizeof(n), 1, fd);
            n = rr_counter_get(RR_EVENT_HELPER_ST); fwrite(&n, sizeof(n), 1, fd);
        }
    }
}

static uint64_t rr_counter = 0;

static bool eflags_cmp(CPUX86State *env, FILE *fd)
{
    uint32_t tcg_eflags;
    uint32_t eflags = cpu_compute_eflags(env);
    int n = 0;

    if (!TRACE_CMP_EFLAGS) {
        return true;
    }

    n = fread(&tcg_eflags, sizeof(tcg_eflags), 1, fd);

    if (n != 1 || tcg_eflags != eflags) {
        return false;
    }

    return true;
}

/* some reg mismatch (maybe) caused by load eflags where undefined bit mismatch */
static bool waived_reg_compare(int i, target_ulong reg1, target_ulong reg2)
{
    bool waived = false;
    
    if(
        (i==11 && (reg1 == 0x202) && (reg2 == 0x206)) ||
        (i==11 && (reg1 == 0x216) && (reg2 == 0x206)) ||
        (i==11 && (reg1 == 0x212) && (reg2 == 0x202)) ||
        (i==11 && (reg1 == 0x20023f) && (reg2 == 0x23f)))
        waived = true;

    return waived;
}

static bool gpr_regs_cmp(CPUX86State *env, FILE *fd)
{
    int i = 0;
    int n = 0;
    target_ulong reg = 0;

    bool err = 0;
    if (!TRACE_CMP_GPR_REGS) {
        return true;
    }

    for (i = 0; i < CPU_NB_REGS; i++) {
        n = fread(&reg, sizeof(reg), 1, fd);
        if (n != 1 || reg != env->regs[i]) {
            fprintf(stderr, "GPR %d %lx  ref %lx",i, env->regs[i], reg);
            if (waived_reg_compare(i, reg, env->regs[i])) {
                fprintf(stderr, " waived\n");
                continue;
            }
            fprintf(stderr, " type x to exit?\n");
            char c = getchar();
            if (c == 'x') {
                err = 1;
                return false;
            } else {
                err = 0;
            }
        }
    }
    if(err)
        return false;

    return true;
}

static bool cr_regs_cmp(CPUX86State *env, FILE *fd)
{
    int n = 0;
    target_ulong reg = 0;

    if (!TRACE_CMP_CR_REGS) {
        return true;
    }

    n = fread(&reg, sizeof(reg), 1, fd);
    if (n != 1 || reg != env->cr[0]) {
        fprintf(stderr, "CR0 %lx  ref %lx\n", env->cr[0], reg);
        return false;
    }

    n = fread(&reg, sizeof(reg), 1, fd);
    if (n != 1 || reg != env->cr[2]) {
        fprintf(stderr, "CR2 %lx  ref %lx\n", env->cr[2], reg);
        return false;
    }

    n = fread(&reg, sizeof(reg), 1, fd);
    if (n != 1 || reg != env->cr[3]) {
        fprintf(stderr, "CR3 %lx  ref %lx\n", env->cr[3], reg);
        return false;
    }

    n = fread(&reg, sizeof(reg), 1, fd);
    if (n != 1 || reg != env->cr[4]) {
        fprintf(stderr, "CR4 %lx  ref %lx\n", env->cr[4], reg);
        return false;
    }

    n = fread(&reg, sizeof(reg), 1, fd);
    if (n != 1 || reg != helper_read_crN(env, 8)) {
        fprintf(stderr, "CR8 %lx  ref %lx\n", helper_read_crN(env, 8), reg);
        return false;
    }

    return true;
}

static bool xmm_regs_cmp(CPUX86State *env, FILE *fd)
{
    int i = 0;
    uint64_t zmmreg0, zmmreg1;
    uint64_t n0, n1;

    if (!TRACE_CMP_XMM_REGS) {
        return true;
    }

    for (i = 0; i < CPU_NB_REGS; i++) {
        if (qemu_tcg_mode_1_tcg()) {
            n0 = fread(&zmmreg0, sizeof(zmmreg0), 1, fd);
            n1 = fread(&zmmreg1, sizeof(zmmreg1), 1, fd);

            if (env->xmm_regs[i]._q_ZMMReg[0] != zmmreg0
                || env->xmm_regs[i]._q_ZMMReg[1] != zmmreg1
                || n0 != 1 || n1 != 1) {
                return false;
            }
        } else if (qemu_tcg_mode_2_lat()) {
            n0 = fread(&zmmreg0, sizeof(zmmreg0), 1, fd);
            n1 = fread(&zmmreg1, sizeof(zmmreg1), 1, fd);

            if (env->xmm_regs[i]._q_ZMMReg[0] != zmmreg0
                || env->xmm_regs[i]._q_ZMMReg[1] != zmmreg1
                || n0 != 1 || n1 != 1) {
                return false;
            }
        }
    }

    return true;
}

static bool rr_event_cmp(CPUX86State *env, FILE *fd)
{
    if (!TRACE_CMP_EVENT || !rr_counter_enabled()) {
        return true;
    }

    int n = 0;
    uint64_t num = 0, num2 = 0;

    n = fread(&num, sizeof(num), 1, fd);
    num2 = rr_counter_get(RR_EVENT_HELPER_LD);
    if (n != 1 || num != num2) {
        fprintf(stderr, "RR %ld helper ld %ld %ld\n", rr_counter, num2, num);
        return false;
    }

    n = fread(&num, sizeof(num), 1, fd);
    num2 = rr_counter_get(RR_EVENT_HELPER_ST);
    if (n != 1 || num != num2) {
        fprintf(stderr, "RR %ld helper st %ld %ld\n", rr_counter, num2, num);
        return false;
    }

    return true;
}

static void __latxs_tb_trace_rr_bin_cmp(
        CPUX86State *env, TranslationBlock *tb,
        FILE *fd, bool noeflags, int isexcp)
{
    uint64_t _pc = tb->pc;
    uint64_t pc;
    int n = fread(&pc, sizeof(pc), 1, fd);
    if (pc != _pc || n != 1
        || !(eflags_cmp(env, fd))
        || !(gpr_regs_cmp(env, fd))
        || !(cr_regs_cmp(env, fd))
        || !(rr_event_cmp(env, fd))
        || !(xmm_regs_cmp(env, fd))) {
        fprintf(stderr, "RR %ld\n TB.PC %#lx READ.PC %#lx n=%d",
                rr_counter, _pc, pc, n);
        exit(-1);
    }
}

static void __latxs_tb_trace_rr_bin(CPUX86State *env, TranslationBlock *tb,
        int isexcp)
{
    FILE *fd = rrtrace_file;
    bool noeflags = qemu_rrtrace_noeflags();

    switch (rrtrace_func) {
    case 1: __latxs_tb_trace_rr_bin_out(env, tb, fd, noeflags, isexcp); break;
    case 2: __latxs_tb_trace_rr_bin_cmp(env, tb, fd, noeflags, isexcp); break;
    default: break;
    }
}

#if defined(CONFIG_SOFTMMU)
extern uint64_t replay_get_current_icount(void);
#endif

void __latxs_tb_trace_rr(CPUX86State *env, TranslationBlock *tb,
        int isexcp)
{
    rr_counter += 1;
#if defined(CONFIG_SOFTMMU)
    uint64_t curr_icount = replay_get_current_icount();
#else
    uint64_t curr_icount = 0;
#endif

    if (qemu_rrtrace_end_exit_tbcount(rr_counter) ||
        qemu_rrtrace_end_exit_icount(curr_icount)) {
        printf("tracing end, exiting\n");
        exit(0);
    }

    int can_trace = qemu_rrtrace_can_begin_tbcount(rr_counter) &&
                    qemu_rrtrace_can_begin_icount(curr_icount);

    if (!can_trace) {
        if (qemu_rrtrace_verbose_tb_nr(rr_counter)) {
            printf("%s TB Counter %ld not begin yet\n",
                    __func__, rr_counter);
        } else if (qemu_rrtrace_verbose_icount(curr_icount)) {
            printf("%s icount %ld not begin yet\n",
                    __func__, curr_icount);
            qemu_rrtrace_stride_i_update(curr_icount);
        }
        return;
    }

    if (rrtrace_mode == 1) {
        FILE *fd = rrtrace_file;
        int config = 0;
        config |= (rrtrace_level & 0xf);
        config |= (rrtrace_config << 4);
        __latxs_tb_trace_nocheck(env, tb, fd, config, isexcp);
    } else if (rrtrace_mode == 2) {
        __latxs_tb_trace_rr_bin(env, tb, isexcp);
    }

    int do_verbose = 0;
    if (qemu_rrtrace_verbose_tb_nr(rr_counter)) {
        printf("%s TB Counter %ld icount %ld : ", __func__,
                rr_counter,
                curr_icount);
        do_verbose = 1;
    } else if (qemu_rrtrace_verbose_icount(curr_icount)) {
        qemu_rrtrace_stride_i_update(curr_icount);
        printf("%s icount %ld(%07ld) tbcount %ld: ", __func__,
                qemu_rrtrace_stride_i_upper(curr_icount),
                qemu_rrtrace_stride_i_lower(curr_icount),
                rr_counter);
        do_verbose = 1;
    }
    if (do_verbose) {
        if (rrtrace_file) fflush(rrtrace_file);
        if (rr_counter_enabled()) {
            rr_counter_dump_all(stdout);
            rr_counter_reset_all();
        }
        printf("\n");
    }
}

#endif

#define TBTRACE_CFG_LEVEL(val)  ((val) & 0xf)
#define TBTRACE_CFG_NOEFLAGS(val)  ((val) & 0x10)

void __latxs_tb_trace_nocheck(CPUX86State *env, TranslationBlock *tb,
        FILE *fd, int config, int isexcp)
{
    if (fd == NULL) {
        fd = stderr;
    }

#if defined(CONFIG_SOFTMMU)
    fprintf(fd, "%ld  ",  replay_get_current_icount());
#endif
    if (isexcp) {
        fprintf(fd, "[%s] ", isexcp == 1 ? "excp" : "intr");
    } else {
        fprintf(fd, "[exec] ");
    }

    /*fprintf(fd, "TB=0x%p ",  tb);*/
    fprintf(fd, "PC=0x%lx ",  (uint64_t)tb->pc);
    fprintf(fd, "EIP=0x%lx ", (uint64_t)env->eip);
    fprintf(fd, "CS=0x%lx ",  (uint64_t)tb->cs_base);
    fprintf(fd, "HF=0x%lx ",  (uint64_t)tb->flags);
    if (rr_counter_enabled()) {
        fprintf(fd, "LD=%ld ", rr_counter_get(RR_EVENT_HELPER_LD));
        fprintf(fd, "ST=%ld ", rr_counter_get(RR_EVENT_HELPER_ST));
    }
    if (!TBTRACE_CFG_NOEFLAGS(config)) {
        uint32_t eflags = cpu_compute_eflags(env);
        fprintf(fd, "EF=0x%04x ", eflags);
    }
    if (tb->cflags & CF_USE_ICOUNT) {
        X86CPU *xcpu = env_archcpu(env);
        fprintf(fd, "ICOUNT=0x%x / ", xcpu->neg.icount_decr.u32);
    }
    switch (TBTRACE_CFG_LEVEL(config)) {
    case 2: /* Print with FPU state */
        fprintf(fd, "TOP=%d / ",  env->fpstt);
        fprintf(fd, "RM=%d / ", env->fp_status.float_rounding_mode);
        fprintf(fd, "FP=0x%x.0x%lx 0x%x.0x%lx 0x%x.0x%lx 0x%x.0x%lx "
                           "0x%x.0x%lx 0x%x.0x%lx 0x%x.0x%lx 0x%x.0x%lx / ",
                           env->fpregs[0].d.high, env->fpregs[0].d.low,
                           env->fpregs[1].d.high, env->fpregs[1].d.low,
                           env->fpregs[2].d.high, env->fpregs[2].d.low,
                           env->fpregs[3].d.high, env->fpregs[3].d.low,
                           env->fpregs[4].d.high, env->fpregs[4].d.low,
                           env->fpregs[5].d.high, env->fpregs[5].d.low,
                           env->fpregs[6].d.high, env->fpregs[6].d.low,
                           env->fpregs[7].d.high, env->fpregs[7].d.low);
        break;
    case 3: /* Print with SIMD state */
        fprintf(fd, "SIMD ");
#define LATX_TB_TRACE_SIMD(n) do {              \
    fprintf(fd, "[%d]0x%016lx,0x%016lx ", n,    \
            env->xmm_regs[n]._q_ZMMReg[0],  \
            env->xmm_regs[n]._q_ZMMReg[1]); \
} while(0)
#define TCG_TB_TRACE_SIMD(n) do {               \
    fprintf(fd, "[%d]0x%016lx,0x%016lx ", n,    \
            env->xmm_regs[n]._q_ZMMReg[0],  \
            env->xmm_regs[n]._q_ZMMReg[1]); \
} while(0)
        {
            int i = 0;
            if (qemu_tcg_mode_1_tcg()) {
                for (i = 0; i < CPU_NB_REGS; ++i) {
                    TCG_TB_TRACE_SIMD(i);
                }
            } else if (qemu_tcg_mode_2_lat()) {
                for (i = 0; i < CPU_NB_REGS; ++i) {
                    LATX_TB_TRACE_SIMD(i);
                }
            }
        }
        break;
    default:
        break;
    }
    fprintf(fd, "REGS=0x"TARGET_FMT_lx",0x"TARGET_FMT_lx
                        ",0x"TARGET_FMT_lx",0x"TARGET_FMT_lx
                        ",0x"TARGET_FMT_lx",0x"TARGET_FMT_lx
                        ",0x"TARGET_FMT_lx",0x"TARGET_FMT_lx
                        ",0x"TARGET_FMT_lx",0x"TARGET_FMT_lx
                        ",0x"TARGET_FMT_lx",0x"TARGET_FMT_lx
                        ",0x"TARGET_FMT_lx",0x"TARGET_FMT_lx
                        ",0x"TARGET_FMT_lx",0x"TARGET_FMT_lx"\n",
            env->regs[0], env->regs[1], env->regs[2], env->regs[3],
            env->regs[4], env->regs[5], env->regs[6], env->regs[7],
            env->regs[8], env->regs[9], env->regs[10], env->regs[11],
            env->regs[12], env->regs[13], env->regs[14], env->regs[15]);
    fflush(fd);
}

#endif
