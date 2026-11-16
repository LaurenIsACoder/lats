/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <poll.h>
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
#include "hamt-dmw.h"
#include "qemu/rcu.h"
#include "qemu/main-loop.h"
#include "qemu/event_notifier.h"
#include "sysemu/cpus.h"
#include "sysemu/sysemu.h"
#include "qemu/error-report.h"

/*
 * Run notification independently of the BQL and main loop: callers such as
 * pause_all_vcpus() may be waiting there for an unpolled TB to exit. Arming
 * a kernel timer does not wake the worker merely to update its poll timeout.
 * Keep the lock alive after shutdown, for producers that passed the first
 * initialized check before the worker joined.
 */
#define ASYNC_IRQ_GRACE_NS (100 * 1000)
#define ASYNC_IRQ_RETRY_NS (1000 * 1000)
static bool async_irq_timer_initialized;
static bool async_irq_flush_pending;
static bool async_irq_grace_pending;
static QemuMutex async_irq_timer_lock;
static int async_irq_timer_fd = -1;
static EventNotifier async_irq_stop;
static int64_t async_irq_deadline;
static QemuThread async_irq_thread;
static pid_t async_irq_pid;
static uid_t async_irq_uid;

static void async_irq_timer_cb(void *opaque);

/* HAMT native helpers may pass a direct-mapped stack to host syscalls. */
static void *async_irq_host_address(void *ptr)
{
    if (hamt_enabled() && hamt_mode_dmw()) {
        return (void *)(uintptr_t)DMW_OUT(ptr);
    }
    return ptr;
}

static int64_t async_irq_now(void)
{
    struct timespec ts;

    /*
     * QEMU_CLOCK_REALTIME uses raw host RDTIME in this tree. Never mix its
     * epoch with the kernel timer's CLOCK_MONOTONIC absolute deadlines.
     */
    if (clock_gettime(CLOCK_MONOTONIC, async_irq_host_address(&ts))) {
        error_report("async IRQ clock_gettime: %s", strerror(errno));
        abort();
    }
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void async_irq_arm(int64_t delay_ns, bool grace)
{
    int64_t deadline;

    /*
     * AOT fork children inherit the descriptors and possibly a held lock,
     * but must never reprogram the parent's timer or take that lock.
     */
    if (!qatomic_load_acquire(&async_irq_timer_initialized) ||
        getpid() != async_irq_pid) {
        return;
    }
    deadline = async_irq_now() + delay_ns;

    /*
     * A HAMT timer interrupt can reenter this producer while its interrupted
     * helper owns the lock. Never block here. An arm owner retains/programs
     * a timer; a reader owner scans all pending CPUs after consuming expiry
     * (or EAGAIN leaves a reprogrammed timer). The only other owner shuts the
     * backend down. The request was published before entering this function.
     * Bypass QSP: its callsite table update is not interrupt-reentrant either.
     */
    if (qemu_mutex_trylock__raw(&async_irq_timer_lock)) {
        return;
    }
    if (qatomic_read(&async_irq_timer_initialized) &&
        (!async_irq_deadline || deadline < async_irq_deadline)) {
        struct itimerspec spec = {
            .it_value = { deadline / 1000000000, deadline % 1000000000 }
        };

        if (timerfd_settime(async_irq_timer_fd, TFD_TIMER_ABSTIME,
                            async_irq_host_address(&spec), NULL)) {
            error_report("async IRQ timerfd_settime: %s", strerror(errno));
            abort();
        }
        async_irq_deadline = deadline;
    }
    if (grace && qatomic_read(&async_irq_timer_initialized) &&
        async_irq_deadline) {
        /*
         * Only an established deadline permits other producers to skip
         * arming. Publish under the reader's lock so an expired timer
         * cannot leave a stale gate behind after its CPU scan.
         */
        qatomic_store_release(&async_irq_grace_pending, true);
    }
    qemu_mutex_unlock(&async_irq_timer_lock);
}

static void async_irq_schedule(CPUState *cpu);


static int async_irq_signal_cpu(CPUState *cpu)
{
    siginfo_t info = { 0 };

    info.si_signo = SIGRTMIN;
    info.si_code = SI_QUEUE;
    info.si_pid = async_irq_pid;
    info.si_uid = async_irq_uid;
    /*
     * A numeric TID avoids dereferencing a pthread descriptor while CPU
     * teardown joins that thread. The handler only uses its current_cpu,
     * so late delivery to a reused TID cannot dereference the old CPU.
     */
    return syscall(SYS_rt_tgsigqueueinfo, async_irq_pid,
                   qatomic_read(&cpu->thread_id), SIGRTMIN, &info) < 0 ?
           errno : 0;
}

static void *async_irq_thread_main(void *opaque)
{
    struct pollfd fds[] = {
        { .fd = async_irq_timer_fd, .events = POLLIN },
        { .fd = event_notifier_get_fd(&async_irq_stop), .events = POLLIN }
    };

    (void)opaque;
    rcu_register_thread();
    while (qatomic_load_acquire(&async_irq_timer_initialized)) {
        int ret = poll(fds, G_N_ELEMENTS(fds), -1);
        bool expired = false;

        if (ret < 0 && errno == EINTR) {
            continue;
        }
        if (ret < 0 || ((fds[0].revents | fds[1].revents) &
                        (POLLERR | POLLHUP | POLLNVAL))) {
            error_report("async IRQ timer poll failed");
            abort();
        }
        if (fds[1].revents & POLLIN) {
            break;
        }
        if (fds[0].revents & POLLIN) {
            uint64_t expirations;
            ssize_t n;

            /*
             * Serialize reading/clearing the old deadline against a producer
             * advancing the timer. EAGAIN may mean it was reprogrammed after
             * poll; do not erase that producer's deadline.
             */
            qemu_mutex_lock(&async_irq_timer_lock);
            if (qatomic_read(&async_irq_timer_initialized)) {
                do {
                    n = read(async_irq_timer_fd, &expirations,
                             sizeof(expirations));
                } while (n < 0 && errno == EINTR);
                if (n == (ssize_t)sizeof(expirations)) {
                    async_irq_deadline = 0;
                    /*
                     * Requests published before this exchange are covered
                     * by the following CPU scan. Later requests may arm a
                     * fresh grace deadline, including during that scan.
                     */
                    (void)qatomic_xchg(&async_irq_grace_pending, false);
                    expired = true;
                } else if (n >= 0 || errno != EAGAIN) {
                    error_report("async IRQ timer read failed");
                    abort();
                }
            }
            qemu_mutex_unlock(&async_irq_timer_lock);
        }
        if (expired) {
            async_irq_timer_cb(NULL);
        }
    }
    rcu_unregister_thread();
    return NULL;
}

static void async_irq_timer_cb(void *opaque)
{
    CPUState *cpu;
    bool pending = false;

    (void)opaque;
    if (!qatomic_load_acquire(&async_irq_timer_initialized)) {
        return;
    }
    cpu_list_lock();
    CPU_FOREACH(cpu) {
        int ret;

        if (!qatomic_read(&cpu->created) || !cpu->thread ||
            qatomic_read(&cpu->stopped) ||
            qatomic_read(&cpu->halted) || !cpu_loop_exit_requested(cpu)) {
            continue;
        }
        ret = async_irq_signal_cpu(cpu);

        if (ret) {
            SIGRTMIN_LOG("%s failed: %d(%s)\n", __func__, ret, strerror(ret));
        }
        pending = true;
    }
    cpu_list_unlock();
    if (pending && qatomic_load_acquire(&async_irq_timer_initialized)) {
        async_irq_arm(ASYNC_IRQ_RETRY_NS, false);
    }
}

static void async_irq_timer_exit(Notifier *notifier, void *data)
{
    (void)notifier;
    (void)data;
    if (getpid() != async_irq_pid) {
        return;
    }
    qemu_mutex_lock(&async_irq_timer_lock);
    qatomic_store_release(&async_irq_timer_initialized, false);
    /*
     * No need to disarm: no producer can rearm after this point, and the
     * worker is joined before either descriptor is closed.
     */
    event_notifier_set(&async_irq_stop);
    qemu_mutex_unlock(&async_irq_timer_lock);
    qemu_thread_join(&async_irq_thread);
    close(async_irq_timer_fd);
    event_notifier_cleanup(&async_irq_stop);
}

static Notifier async_irq_exit_notifier = { .notify = async_irq_timer_exit };

static void async_irq_timer_init(void)
{
    async_irq_pid = getpid();
    async_irq_uid = getuid();
    qemu_mutex_init(&async_irq_timer_lock);
    async_irq_timer_fd = timerfd_create(CLOCK_MONOTONIC,
                                        TFD_NONBLOCK | TFD_CLOEXEC);
    if (async_irq_timer_fd < 0 || event_notifier_init(&async_irq_stop, 0)) {
        error_report("cannot initialize async IRQ timer");
        exit(EXIT_FAILURE);
    }
    qatomic_store_release(&async_irq_timer_initialized, true);
    qemu_thread_create(&async_irq_thread, "lats-irq-notify",
                       async_irq_thread_main, NULL, QEMU_THREAD_JOINABLE);
    qemu_add_exit_notifier(&async_irq_exit_notifier);
}

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

    if (n == 0 && (tb->bool_flags & IS_HRAS_CALL)) {
        unsigned bytes = tb->jmp_reset_offset[n] - tb->jmp_insn_offset[n];
        if (bytes == 4) {
            tb->signal_unlink[n] = qatomic_read((uint32_t *)jmp_inst_ptr);
            qatomic_set((uint32_t *)jmp_inst_ptr, (uint32_t)inst);
        } else {
            g_assert(bytes == 8);
            tb->signal_unlink[n] = qatomic_read((uint64_t *)jmp_inst_ptr);
            qatomic_set((uint64_t *)jmp_inst_ptr, inst);
        }
        flush_idcache_range((uintptr_t)jmp_inst_ptr, (uintptr_t)jmp_inst_ptr, bytes);
    } else {
        tb->signal_unlink[n] = qatomic_read((uint64_t *)jmp_inst_ptr);
        qatomic_set((uint64_t *)jmp_inst_ptr, inst);
        flush_idcache_range((uintptr_t)jmp_inst_ptr, (uintptr_t)jmp_inst_ptr, 8);
    }
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
    int saved_errno = errno;

    if (IRQ_CHECK_ASYNC_NOTIFY != option_int_opt) {
        goto out;
    }

    if (signo != SIGRTMIN) {
        SIGRTMIN_LOG("%s signo != SIGRTMIN\n", __func__);
        goto out;
    }

    if (!puc) {
        SIGRTMIN_LOG("%s puc is NULL\n", __func__);
        goto out;
    }

    CPUState *cpu = current_cpu;
    if (!cpu || !cpu->curr_exec_tb || !cpu_loop_exit_requested(cpu)) {
        SIGRTMIN_LOG("%s cpu is NULL\n", __func__);
        goto out;
    }

    ucontext_t *uc = puc;
    TranslationBlock *tb = NULL;

    /*
     * HAMT only needs to interrupt KVM_RUN. Looking up a TB from a
     * handler can reenter the interrupted thread's code-region lock.
     */
    if (hamt_enabled() || !in_code_gen_buffer((void *)UC_PC(uc))) {
        goto out;
    }
    tb = tcg_tb_lookup(UC_PC(uc));

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

out:
    errno = saved_errno;
}

void lats_init_rr_thread_sigrtmin(CPUState *cpu)
{
    sigset_t set, oldset;
    struct sigaction act;
    int ret;

    if (IRQ_CHECK_ASYNC_NOTIFY != option_int_opt) {
        return;
    }
#if defined(CONFIG_LATX_TCG_MODE)
    return;
#endif

    /* Install the process-wide action before unblocking this thread. */
    sigemptyset(&set);
    sigaddset(&set, SIGRTMIN);

    memset(&act, 0, sizeof(act));
    
    act.sa_sigaction = latxs_rr_sigrtmin_handler;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);

    ret = sigaction(SIGRTMIN, &act, NULL);
    if (ret < 0) {
        error_report("cannot install SIGRTMIN handler: %s", strerror(errno));
        exit(-1);
    }

    ret = pthread_sigmask(SIG_UNBLOCK, &set, &oldset);
    if (ret) {
        error_report("cannot unblock SIGRTMIN: %s", strerror(ret));
        exit(-1);
    }

    /* vCPU startup holds the BQL, serializing this one-time initialization. */
    if (!qatomic_read(&async_irq_timer_initialized)) {
        async_irq_timer_init();
    }
    /* Cover a request published before this CPU's notifier was ready. */
    async_irq_schedule(cpu);

    SIGRTMIN_LOG("%s cpu%d thread %x set signal SIGRTMIN handler\n",
                __func__, cpu->cpu_index, (unsigned int)pthread_self());
}

static void async_irq_schedule(CPUState *cpu)
{
    if (IRQ_CHECK_ASYNC_NOTIFY != option_int_opt) {
        return;
    }

    if (!cpu || !cpu->thread) {
        return;
    }

    if (!cpu_loop_exit_requested(cpu)) {
        /* A polling exit may already have consumed the published request. */
        return;
    }

    /*
     * Always retain the independent fallback, including for self-generated
     * requests. A non-vCPU producer may flush the first notification after
     * releasing the BQL. Repeated requests never postpone the deadline.
     */
    if (qatomic_load_acquire(&async_irq_timer_initialized)) {
        qatomic_store_release(&async_irq_flush_pending, true);
        /*
         * A short-lived request may already have been consumed when the
         * next one arrives. Share the outstanding grace period across
         * those requests instead of reading the clock and taking the
         * timer lock on each transition. The reader clears this gate
         * before scanning CPUs; retries retain their independent timer.
         */
        if (!qatomic_load_acquire(&async_irq_grace_pending)) {
            async_irq_arm(ASYNC_IRQ_GRACE_NS, true);
        }
    }
}

void lats_notify_after_iothread_unlock(void)
{
    CPUState *cpu;

    if (option_int_opt != IRQ_CHECK_ASYNC_NOTIFY || current_cpu ||
        !qatomic_load_acquire(&async_irq_timer_initialized) ||
        !qatomic_xchg(&async_irq_flush_pending, false)) {
        return;
    }
    g_assert(!qemu_mutex_iothread_locked());

    /*
     * Software timer/device callbacks publish under the BQL. Wake executing
     * vCPUs once the producer releases it, so they can handle the request
     * without first waiting for the private timer thread (or spinning on a
     * lock still held by their notifier). Keep that thread's armed timer:
     * a signal can find a helper PC, race a return, or fail to be queued.
     * Only non-vCPU contexts send here; no guest register state is live.
     */
    cpu_list_lock();
    CPU_FOREACH(cpu) {
        if (!qatomic_read(&cpu->created) || !cpu->thread ||
            qatomic_read(&cpu->stopped) || qatomic_read(&cpu->halted) ||
            !qatomic_read(&cpu->curr_exec_tb) ||
            !cpu_loop_exit_requested(cpu)) {
            continue;
        }
        async_irq_signal_cpu(cpu);
    }
    cpu_list_unlock();
}

void lats_request_tb_exit(CPUState *cpu)
{
    if (option_int_opt != IRQ_CHECK_ASYNC_NOTIFY) {
        qatomic_set(&cpu->icount_decr_ptr->u16.high, -1);
        return;
    }

    /*
     * Publish before notifying, but only arm the timer on the transition
     * from no request to pending. A separate load/store could miss a
     * concurrent vCPU clear and lose the wakeup. The worker retries a
     * request that is already pending without another producer-side lock.
     */
    if (qatomic_xchg(&cpu->icount_decr_ptr->u16.high, -1) == 0) {
        async_irq_schedule(cpu);
    }
}
