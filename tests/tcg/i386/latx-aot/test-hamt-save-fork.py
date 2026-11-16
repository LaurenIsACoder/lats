#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise the host fake-fork branch and libc atfork callbacks."""
from pathlib import Path
import json
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[4]
text = (root / "target/i386/hamt/dune-loop.c").read_text()
start = text.index("        if (vcpu->fake_fork) {")
end = text.index("\n        if (sysno == SYS_KEXEC_LOAD)", start)
branch = text[start:end]
save_text = (root / 'target/i386/latx/sbt/sys-aot-save.c').read_text()
start = save_text.index('    /* The ordinary host path')
end = save_text.index('\n    }\n    if (!job->shutdown)', start)
host_branch = save_text[start:end]
host_branch = host_branch.replace('        pid = rcu_fork_snapshot();',
                                  '        pid_t pid = rcu_fork_snapshot();', 1)
host_branch = host_branch.replace('aot_save_child_main(job);',
                                  'vcpu->fake_fork_fn(vcpu->fake_fork_data);'
                                  '\n        _exit(0);')
host_branch += ('\n    vcpu->fake_fork_pid = pid > 0 ? pid : -1;'
                '\n    vcpu->fake_fork = 0;\n')
flush_text = (root / 'accel/tcg/tb-maint.c').read_text()
start = flush_text.index('void tb_flush_mtcc(')
flush = flush_text[start:flush_text.index('\n}', start) + 2]
flush_test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct { bool serial; } CPUState;
typedef uint64_t run_on_cpu_data;
static struct { unsigned depth; } reader;
static struct { unsigned n, tb_flush_count_mtcc[2]; } tb_ctx = {2, {0, 0}};
static int direct, queued, reader_checks;
static int option_sys_aot, option_aot_save;
#define get_ptr_rcu_reader() (reader_checks++, &reader)
#define qemu_tcg_flush_full() true
#define qemu_tcg_mtcc_enabled() true
#define tcg_enabled() true
#define qatomic_read(p) (*(p))
#define RUN_ON_CPU_HOST_ULONG(v) (v)
#define cpu_in_serial_context(cpu) ((cpu)->serial)
static void do_tb_flush_mtcc(CPUState *cpu, run_on_cpu_data data)
{ direct++; }
static void tb_flush(CPUState *cpu) { assert(0); }
static void async_safe_run_on_cpu(CPUState *cpu,
    void (*fn)(CPUState *, run_on_cpu_data), run_on_cpu_data data)
{ assert(fn == do_tb_flush_mtcc); queued++; }
''' + flush + r'''
int main(void)
{
    for (option_sys_aot = 0; option_sys_aot < 2; option_sys_aot++) {
        for (option_aot_save = 0; option_aot_save < 2; option_aot_save++) {
            bool can_fork = option_sys_aot && option_aot_save;

            for (int serial = 0; serial < 2; serial++) {
                for (unsigned depth = 0; depth < 3; depth++) {
                    CPUState cpu = {serial};
                    bool run_direct = serial && (!can_fork || !depth);

                    reader.depth = depth;
                    direct = queued = reader_checks = 0;
                    tb_flush_mtcc(&cpu, 0);
                    assert(direct == run_direct);
                    assert(queued == !run_direct);
                    assert(reader_checks == (serial && can_fork));
                }
            }
        }
    }
    return 0;
}
'''
source = r'''
#include "qemu/osdep.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdatomic.h>
static int prepared, parent_done, child_done;
static bool held_on_entry;
static atomic_bool stop_worker;
static atomic_uint allocations;
static atomic_bool sync_started, sync_finished;
static atomic_bool consumer_waiting;
static atomic_uint reclaimed;
static struct rcu_head pending[40];
static pid_t original_pid;
static unsigned snapshot_reclaimed;
static bool fail_fork;
static int initial_atfork_depth;
int test_rcu_atfork_depth(void);
bool test_rcu_fork_snapshot(void);
static void check_fork_state(void)
{
    assert(test_rcu_atfork_depth() == initial_atfork_depth);
    assert(!test_rcu_fork_snapshot());
}
pid_t __real_fork(void);
pid_t __wrap_fork(void);
pid_t __wrap_fork(void)
{
    if (fail_fork) {
        errno = EAGAIN;
        return -1;
    }
    return __real_fork();
}
static void *sync_worker(void *opaque)
{
    rcu_register_thread();
    atomic_store(&sync_started, true);
    synchronize_rcu();
    atomic_store(&sync_finished, true);
    rcu_unregister_thread();
    return NULL;
}
static void *allocate_worker(void *opaque)
{
    while (!atomic_load(&stop_worker)) {
        void *p = malloc(4096);
        assert(p);
        memset(p, 0xa5, 4096);
        p = realloc(p, 131072);
        assert(p);
        free(p);
        atomic_fetch_add(&allocations, 1);
    }
    return NULL;
}
static QemuMutex bql;
static __thread bool bql_locked;
bool qemu_mutex_iothread_locked(void) { return bql_locked; }
void qemu_mutex_lock_iothread_impl(const char *file, int line)
{
    atomic_store(&consumer_waiting, true);
    qemu_mutex_lock(&bql);
    bql_locked = true;
}
void qemu_mutex_unlock_iothread(void)
{
    bql_locked = false;
    qemu_mutex_unlock(&bql);
}
static void prepare(void)
{
    assert(qemu_mutex_iothread_locked());
    assert(get_ptr_rcu_reader()->depth == 0);
    assert(get_ptr_rcu_reader()->ctr == 0);
    snapshot_reclaimed = atomic_load(&reclaimed);
    prepared++;
}
static void parent(void) { parent_done++; }
static void child(void) { child_done++; }
static void reclaim(struct rcu_head *node)
{
    /* Inherited deferred frees must remain the parent's responsibility. */
    assert(getpid() == original_pid);
    atomic_fetch_add(&reclaimed, 1);
}
struct cpu {
    int fake_fork;
    pid_t fake_fork_pid;
    void (*fake_fork_fn)(void *);
    void *fake_fork_data;
    uint64_t syscall_parameter[8];
};
static void save(void *opaque)
{
    alarm(3);
    check_fork_state();
    assert(qemu_mutex_iothread_locked() == held_on_entry);
    assert(prepared == 1 && child_done == 1 && parent_done == 0);
    assert(*(int *)opaque == 42);
    assert(get_ptr_rcu_reader()->depth == 0);
    assert(get_ptr_rcu_reader()->ctr == 0);
    rcu_read_lock();
    rcu_read_unlock();
    synchronize_rcu();
    drain_call_rcu();
    assert(atomic_load(&reclaimed) == snapshot_reclaimed);
    void *p = malloc(1024);
    assert(p);
    p = realloc(p, 12582912);
    assert(p);
    memset(p, 1, 12582912);
    free(p);
}
static void run(struct cpu *vcpu)
{
    while (vcpu->fake_fork) {
''' + branch + r'''
    }
}
int main(int argc, char **argv)
{
    pthread_t worker, synchronizer;
    held_on_entry = argc > 1 && !strcmp(argv[1], "bql-held");
    bool atfork_enabled = argc > 2;
    original_pid = getpid();
    int value = 42, status;
    struct cpu cpu = { .fake_fork = 1, .fake_fork_fn = save,
                      .fake_fork_data = &value,
                      .syscall_parameter = {1, 2, 3, 4, 5, 6, 7, 8} };
    uint64_t params[8];
    memcpy(params, cpu.syscall_parameter, sizeof(params));
    assert(pthread_atfork(prepare, parent, child) == 0);
    qemu_mutex_init(&bql);
    if (!atfork_enabled) {
        /* Match qemu_maybe_daemonize(), including non-daemon startup. */
        rcu_disable_atfork();
    }
    initial_atfork_depth = test_rcu_atfork_depth();
    assert(initial_atfork_depth == (atfork_enabled ? 1 : 0));
    assert(pthread_create(&worker, NULL, allocate_worker, NULL) == 0);
    while (atomic_load(&allocations) < 100) {
        sched_yield();
    }
    rcu_read_lock();
    assert(get_ptr_rcu_reader()->depth != 0);
    assert(pthread_create(&synchronizer, NULL, sync_worker, NULL) == 0);
    while (!atomic_load(&sync_started)) {
        sched_yield();
    }
    assert(!atomic_load(&sync_finished));
    /* The queued flush runs only after cpu_exec leaves its read section. */
    rcu_read_unlock();
    alarm(10);
    qemu_mutex_lock_iothread();
    atomic_store(&consumer_waiting, false);
    for (unsigned i = 0; i < G_N_ELEMENTS(pending); i++) {
        call_rcu1(&pending[i], reclaim);
    }
    /* The consumer has removed its batch count and now waits for BQL. */
    while (!atomic_load(&consumer_waiting)) {
        sched_yield();
    }
    assert(atomic_load(&reclaimed) == 0);
    if (!held_on_entry) {
        qemu_mutex_unlock_iothread();
    }
    fail_fork = true;
    run(&cpu);
    assert(cpu.fake_fork == 0 && cpu.fake_fork_pid == -1);
    assert(errno == EAGAIN);
    check_fork_state();
    assert(qemu_mutex_iothread_locked() == held_on_entry);
    assert(prepared == 0 && parent_done == 0 && child_done == 0);
    fail_fork = false;
    cpu.fake_fork = 1;
    run(&cpu);
    check_fork_state();
    assert(pthread_join(synchronizer, NULL) == 0);
    assert(atomic_load(&sync_finished));
    assert(qemu_mutex_iothread_locked() == held_on_entry);
    if (held_on_entry) {
        qemu_mutex_unlock_iothread();
    }
    assert(cpu.fake_fork == 0 && cpu.fake_fork_pid > 0);
    assert(prepared == 1 && parent_done == 1 && child_done == 0);
    assert(!memcmp(params, cpu.syscall_parameter, sizeof(params)));
    assert(waitpid(cpu.fake_fork_pid, &status, 0) == cpu.fake_fork_pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    drain_call_rcu();
    assert(atomic_load(&reclaimed) == G_N_ELEMENTS(pending));
    rcu_read_lock();
    rcu_read_unlock();
    synchronize_rcu();
    atomic_store(&stop_worker, true);
    assert(pthread_join(worker, NULL) == 0);
    return 0;
}
'''
build = (root / (sys.argv[1] if len(sys.argv) > 1 else 'build')).resolve()
rcu_text = (root / 'util/rcu.c').read_text()
# Observe private state in the test copy without adding a production API.
rcu_probe = r'''
int test_rcu_atfork_depth(void);
bool test_rcu_fork_snapshot(void);
int test_rcu_atfork_depth(void) { return atfork_depth; }
bool test_rcu_fork_snapshot(void) { return fork_snapshot; }
'''
entries = json.loads((build / 'compile_commands.json').read_text())
entry = next(e for e in entries if e['file'].endswith('/util/rcu.c'))
args = shlex.split(entry['command'])
flags = []
i = 0
while i < len(args):
    if args[i] in ('-o', '-c', '-MF', '-MQ', '-MT'):
        i += 2
    elif args[i] == '-MD':
        i += 1
    else:
        flags.append(args[i])
        i += 1
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "test.c"
    rcu_src = Path(tmp) / 'rcu.c'
    rcu_src.write_text(rcu_text + rcu_probe)
    exe = Path(tmp) / "test"
    src.write_text(flush_test)
    subprocess.run(['cc', '-Wall', '-Werror', str(src), '-o', str(exe)],
                   check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=15)
    gate = ('(!option_sys_aot || !option_aot_save ||\n'
            '             get_ptr_rcu_reader()->depth == 0)')
    for replacement in ('true', 'get_ptr_rcu_reader()->depth == 0',
                        '(!option_sys_aot || '
                        'get_ptr_rcu_reader()->depth == 0)',
                        '(!option_aot_save || '
                        'get_ptr_rcu_reader()->depth == 0)'):
        bad_flush = flush_test.replace(gate, replacement)
        assert bad_flush != flush_test
        src.write_text(bad_flush)
        subprocess.run(['cc', str(src), '-o', str(exe)], check=True,
                       timeout=60)
        assert subprocess.run([str(exe)], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,
                              timeout=15).returncode != 0
    src.write_text(source)
    libs = ['-Wl,--wrap=fork'] + shlex.split(subprocess.check_output(
        ['pkg-config', '--libs', 'glib-2.0', 'gthread-2.0'], text=True))
    subprocess.run(flags + [str(src), str(rcu_src), '-Wl,--start-group',
                    str(build / 'libqemuutil.a'), str(build / 'libqom.fa'),
                    '-Wl,--end-group', '-pthread', '-o', str(exe)] + libs,
                   cwd=entry['directory'], check=True, timeout=60)
    modes = [['bql-free'], ['bql-held'],
             ['bql-free', 'atfork-enabled'], ['bql-held', 'atfork-enabled']]
    for iteration in range(5):
        for mode in modes:
            subprocess.run([str(exe)] + mode, check=True, timeout=15)
    src.write_text(source.replace(branch, host_branch))
    subprocess.run(flags + [str(src), str(rcu_src), '-Wl,--start-group',
                    str(build / 'libqemuutil.a'), str(build / 'libqom.fa'),
                    '-Wl,--end-group', '-pthread', '-o', str(exe)] + libs,
                   cwd=entry['directory'], check=True, timeout=60)
    for mode in modes:
        subprocess.run([str(exe)] + mode, check=True, timeout=15)
    bad_host = host_branch.replace('        qemu_mutex_lock_iothread();', '')
    bad_host = bad_host.replace('        qemu_mutex_unlock_iothread();', '')
    assert bad_host != host_branch
    src.write_text(source.replace(branch, bad_host))
    subprocess.run(flags + [str(src), str(rcu_src), '-Wl,--start-group',
                    str(build / 'libqemuutil.a'), str(build / 'libqom.fa'),
                    '-Wl,--end-group', '-pthread', '-o', str(exe)] + libs,
                   cwd=entry['directory'], check=True, timeout=60)
    assert subprocess.run([str(exe)], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=15).returncode != 0
    bad_source = source.replace(
        '                qemu_mutex_lock_iothread();', '')
    bad_source = bad_source.replace(
        '                qemu_mutex_unlock_iothread();', '')
    assert bad_source != source
    src.write_text(bad_source)
    subprocess.run(flags + [str(src), str(rcu_src), '-Wl,--start-group',
                    str(build / 'libqemuutil.a'), str(build / 'libqom.fa'),
                    '-Wl,--end-group', '-pthread', '-o', str(exe)] + libs,
                   cwd=entry['directory'], check=True, timeout=60)
    assert subprocess.run([str(exe)], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=15).returncode != 0
    mutations = {
        'disabled-atfork': rcu_text.replace('    rcu_enable_atfork();', ''),
        'inherited-queue': rcu_text.replace('    if (fork_snapshot) {',
                                           '    if (false) {'),
        'failure-mode-restore': rcu_text.replace(
            '    rcu_disable_atfork();\n    fork_snapshot = false;',
            '    if (pid >= 0) { rcu_disable_atfork(); }\n'
            '    fork_snapshot = false;'),
        'parent-mode-restore': rcu_text.replace(
            '    rcu_disable_atfork();\n    fork_snapshot = false;',
            '    if (pid <= 0) { rcu_disable_atfork(); }\n'
            '    fork_snapshot = false;'),
        'child-mode-restore': rcu_text.replace(
            '    rcu_disable_atfork();\n    fork_snapshot = false;',
            '    if (pid != 0) { rcu_disable_atfork(); }\n'
            '    fork_snapshot = false;'),
        'snapshot-marker-restore': rcu_text.replace(
            '    fork_snapshot = false;', ''),
    }
    for name, mutated in mutations.items():
        assert mutated != rcu_text
        rcu_src.write_text(mutated + rcu_probe)
        for host_path in (branch, host_branch):
            src.write_text(source.replace(branch, host_path))
            subprocess.run(flags + [str(src), str(rcu_src),
                           '-Wl,--start-group',
                           str(build / 'libqemuutil.a'),
                           str(build / 'libqom.fa'), '-Wl,--end-group',
                           '-pthread', '-o', str(exe)] + libs,
                           cwd=entry['directory'], check=True, timeout=60)
            for mode in (['bql-held'], ['bql-held', 'atfork-enabled']):
                result = subprocess.run([str(exe)] + mode,
                                        stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, timeout=15)
                assert result.returncode != 0, (name, mode)
print("PASS: HAMT and ordinary host fork; concurrent RCU and allocation; "
      "both BQL entry states; AOT/save-gated RCU dispatch; "
      "disabled/enabled atfork; pending callbacks retained by parent; "
      "parent/child/failure callback mode and marker restored; "
      "missing RCU/BQL and queue reset protection rejected")
