/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <sys/syscall.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <fenv.h>
#include <math.h>
#include "common.h"
#include "lsenv.h"
#include "latx-config.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "qemu/cacheflush.h"
#include "accel/tcg/internal.h"
#include "loongarch-extcontext.h"
#include "latx-sigrt-sys.h"
#include "latx-ras.h"
#include "latx-hamt.h"

static void temp_reset_jmp_target(TranslationBlock *tb, int n)
{
    if (tb->link_opt[n] != 2 || !tb->int_opt[n]) {
        return;
    }

    uint64_t inst = tb->jmp_inst_hold[n];
    void *jmp_inst_ptr;

    if (tb->jmp_cp_data[n] & TB_JMP_CP_DATA_LINKED) {
        int jmp_cp_off = tb->jmp_cp_data[n] & TB_JMP_CP_DATA_OFF;
        jmp_inst_ptr = (void *)((uint64_t)tb->tc.ptr +
                (tb->jmp_insn_offset[n] - (jmp_cp_off << 2)));
    } else {
        jmp_inst_ptr = (void *)((uint64_t)tb->tc.ptr +
            tb->jmp_insn_offset[n]);
    }

    tb->signal_unlink[n] = qatomic_read((uint64_t *)jmp_inst_ptr);
    qatomic_set((uint64_t *)jmp_inst_ptr, inst);
    flush_idcache_range((uintptr_t)jmp_inst_ptr, (uintptr_t)jmp_inst_ptr, 8);
}

static void signal_unlink_tb(TranslationBlock *tb)
{
    /*
     * Unlink direct-jump TBs.
     */
    if (!qemu_tcg_mode_2_lat()) {
        return;
    }
    if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
        temp_reset_jmp_target(tb, 0);
    }
    if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
        temp_reset_jmp_target(tb, 1);
    }
    /*
     * TODO: indirect-jump TBs.
     */
}

static void latxs_rr_sigrtmin_handler(int signo, siginfo_t *info, void *puc)
{
    if (IRQ_CHECK_ASYNC_NOTIFY != option_int_opt) {
        return;
    }

    if (signo != SIGRTMIN) {
        SIGRTMIN_LOG("%s signo != SIGRTMIN\n", __func__);
        return;
    }

    if (!puc) {
        SIGRTMIN_LOG("%s puc is NULL\n", __func__);
        return;
    }

    CPUState *cpu = (CPUState *)info->si_value.sival_ptr;
    if (!cpu) {
        SIGRTMIN_LOG("%s cpu is NULL\n", __func__);
        return;
    }

    ucontext_t *uc = puc;
    TranslationBlock *tb = NULL;

    if (hamt_enabled()) {
        uint64_t kvm_pc = hamt_kvm_cpu_read_pc(cpu);
        kvm_pc = kvm_pc & ~(0xfUL << 60);
        tb = tcg_tb_lookup(kvm_pc);
    } else {
        tb = tcg_tb_lookup(UC_PC(uc));
    }

    SIGRTMIN_LOG("%s  vcpu%d\n", __func__, cpu->cpu_index);
    if (tb) {
        signal_unlink_tb(tb);
    }

    /*
     * Opt:
     *   If the vCPU receives the SIGRTMIN again from another thread
     *   while handling a previous SIGRTMIN, there is no need to process
     *   it again. Clear all pending SIGRTMIN signals.
     */
    sigset_t pending;
    sigemptyset(&pending);
    sigaddset(&pending, SIGRTMIN);
    while (sigtimedwait(&pending, NULL,
                        &(struct timespec){0, 0}) == SIGRTMIN) {
    }

#ifdef CONFIG_LATX_DEBUG
    sigset_t curset;
    pthread_sigmask(SIG_SETMASK, NULL, &curset);
    assert(sigismember(&curset, SIGRTMIN));
#endif

    return;
}

void lats_init_rr_thread_sigrtmin(CPUState *cpu)
{
    if (IRQ_CHECK_ASYNC_NOTIFY != option_int_opt) {
        return;
    }
#if defined(CONFIG_LATX_TCG_MODE)
    return;
#endif

    /* 1. unblock the signal for vCPU thread */
    sigset_t set, oldset;
    sigemptyset(&set);
    sigaddset(&set, SIGRTMIN);

    int ret = pthread_sigmask(SIG_UNBLOCK, &set, &oldset);
    if (ret < 0) {
        fprintf(stderr, "[SIGRTMIN] unblock SIGRTMIN failed\n");
        exit(-1);
    }

    /* 2. set the handler for signal */
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    
    act.sa_sigaction = latxs_rr_sigrtmin_handler;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);

    ret = sigaction(SIGRTMIN, &act, NULL);
    if (ret < 0) {
        fprintf(stderr, "[SIGRTMIN] set handler failed\n");
        exit(-1);
    }

    SIGRTMIN_LOG("%s cpu%d thread %x set signal SIGRTMIN handler\n",
                __func__, cpu->cpu_index, (unsigned int)pthread_self());
}

/*
 * Note: The lats_send_sigrtmin() function must be called
 * before setting the icount_decr flag.
 */
void lats_send_sigrtmin(CPUState *cpu)
{
    pthread_t vcpu_thread;
    union sigval val;

    if (IRQ_CHECK_ASYNC_NOTIFY != option_int_opt) {
        return;
    }

    if (!cpu || !cpu->thread) {
        return;
    }

    if (cpu_loop_exit_requested(cpu)) {
        /*
         * Opt:
         *   Since this function is called before setting icount_decr
         *   to -1, if icount_decr is already -1 here, there's no need to
         *   send the signal.
         */
        //SIGRTMIN_LOG("%s already send to vcpu%d\n", __func__, cpu->cpu_index);
        return;
    }

    if (qemu_cpu_is_self(cpu)) {
        /*
         * After enabling HAMT, the vCPU syscall flow becomes:
         *     guest hypercall -> KVM -> ioctl return -> do syscall ->
         *     ioctl(RUN) -> KVM -> guest
         * Then pthread_sigqueue() fails with EINTR.
         * vCPU self-signalling path can be optimized away, so we don't
         * care about the root cause for now. Just record the issue here.
         */
        return;
    }

    vcpu_thread = cpu->thread->thread;
    val.sival_ptr = cpu;

    int ret = pthread_sigqueue(vcpu_thread, SIGRTMIN, val);
    if (ret) {
        SIGRTMIN_LOG("%s failed: errno %d(%s)\n", __func__, errno, strerror(errno));
    } else {
        SIGRTMIN_LOG("%s send to cpu%d\n", __func__, cpu->cpu_index);
    }
}
