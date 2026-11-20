#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check background shutdown and inherited FDs with a slow save worker."""
from pathlib import Path
import socket
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[4]
source = (root / 'target/i386/latx/sbt/sys-aot-save.c').read_text()


def function(signature):
    start = source.index(signature + '\n{')
    pos = source.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
typedef struct { int core_tb_num, user_tb_num; } AOTSaveContext;
typedef struct { int mtcc_id; bool has_core, has_user; } AOTFlushSaveTrigger;
'''
for name in ('AOTFlushSaveJob', 'AOTRuntimeSaveState'):
    start = source.index('typedef struct ' + name)
    end = source.index('} ' + name + ';', start) + len('} ' + name + ';')
    program += source[start:end] + '\n'
program += r'''
#define AOT_CORE 1
#define AOT_USER 2
#define AOT_CHILD_EXIT_BASE 64
#define AOT_CHILD_MERGE_FAILED 4
#define qatomic_set(p,v) (*(p) = (v))
static int aot_merge_shutting_down, in_aot_translate, snapshot, fail_runtime;
static int option_wait_aot = 0;
static bool aot_runtime_save_child;
static char *aot_file_path;
static int option_verbose;
static void aot_verbose_log(const char *fmt, ...)
{
    va_list ap;

    if (!option_verbose) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
static void *offline_cpu = (void *)1, *global_offline_cpu = (void *)1;
static void *lsenv = (void *)1, *global_lsenv = (void *)1;
static void *tu_data = (void *)1, *global_tu_data = (void *)1;
static void *aot_tu_merge_cpu;
static AOTSaveContext context;
static AOTRuntimeSaveState runtime = { .completion_fd = -1 };
static AOTSaveContext *aot_save_state_context(void) { return &context; }
static AOTRuntimeSaveState *aot_save_state_runtime(void) { return &runtime; }
static void aot_save_fatal(void) { _exit(1); }
static void aot_merger_stop(void) { }
static void aot_save_capture_runtime_context(void) { }
static bool aot_poll_save_child(void) { return true; }
static void aot_wait_save_child(void) { }
static void aot_save_run_sync(int mtcc_id) { (void)mtcc_id; }
typedef struct { int depth; } RCUReader;
static bool bql_locked = true;
static bool qemu_mutex_iothread_locked(void) { return bql_locked; }
static void qemu_mutex_lock_iothread(void) { bql_locked = true; }
static void qemu_mutex_unlock_iothread(void) { bql_locked = false; }
static RCUReader *get_ptr_rcu_reader(void)
{
    static RCUReader reader;
    return &reader;
}
static pid_t rcu_fork_snapshot(void) { return fork(); }
static bool aot_flush_save_trigger_prepare(void) { return true; }
static void tu_trees_reset(void) { }
static void aot_save_context_begin_translate(void) { in_aot_translate = 1; }
static void aot_save_context_free_tb(AOTSaveContext *ctx) { }
static bool aot_cross_page_tu_enabled(void) { return true; }
static bool get_tb(AOTSaveContext *ctx, int mtcc, bool nonblocking)
{
    if (!aot_merge_shutting_down) {
        sleep(3);
        if (fail_runtime) return false;
    }
    ctx->user_tb_num = snapshot;
    return true;
}
static bool aot_save_context_select_flush_outputs(AOTSaveContext *ctx,
                                                 bool *core, bool *user)
{
    *core = false; *user = true; return true;
}
static void generate_aot_file(AOTSaveContext *ctx, const char *name,
                              int count, int mode)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%sresult-%d", aot_file_path, count);
    FILE *file = fopen(path, "w");
    assert(file);
    fprintf(file, "%d\n", count);
    fclose(file);
}
static int aot_merge_all(const char *name, int mode)
{
    if (aot_merge_shutting_down) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%sresult-1", aot_file_path);
        assert(fail_runtime || access(path, F_OK) == 0);
    }
    return 0;
}
static pid_t aot_start_save_child(AOTFlushSaveJob *job);
'''
for signature in (
    'static void aot_flush_save_trigger_init(AOTFlushSaveTrigger *trigger,\n'
    '        int mtcc_id)',
    'static void aot_save_child_detach(AOTFlushSaveJob *job)',
    'static void aot_save_child_main(void *opaque)',
    'static pid_t aot_start_save_child(AOTFlushSaveJob *job)',
    'void aot_save_on_shutdown(void)',
):
    program += function(signature) + '\n'
program += r'''
int main(int argc, char **argv)
{
    aot_file_path = argv[1];
    fail_runtime = argc > 2 && strcmp(argv[2], "fail") == 0;
    option_wait_aot = argc > 3 && strcmp(argv[3], "sync") == 0;
    option_verbose = 1;
    snapshot = 1;
    AOTFlushSaveJob job = { .ctx = &context };
    runtime.child_pid = aot_start_save_child(&job);
    assert(runtime.child_pid > 0);
    snapshot = 2; /* TBs recorded after the first snapshot must survive. */
    aot_save_on_shutdown();
    _exit(0);
}
'''
with tempfile.TemporaryDirectory(prefix='aot-shutdown-') as directory:
    work = Path(directory)
    binary = work / 'test'
    command = ['cc', '-x', 'c', '-Wall', '-Werror', '-o', str(binary), '-']
    subprocess.run(command,
                   input=program, text=True, check=True)
    for failure in (False, True):
        cache = work / str(failure)
        cache.mkdir()
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        address = listener.getsockname()
        args = [str(binary), str(cache) + '/'] + (['fail'] if failure else [])
        start = time.monotonic()
        proc = subprocess.Popen(args, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,
                                pass_fds=(listener.fileno(),))
        listener.close()
        assert proc.wait(timeout=2) == 0
        assert time.monotonic() - start < 2
        # The VM parent exits while the worker still owns the output pipe.
        assert not (cache / 'result-2').exists()
        with socket.socket() as probe:
            # The parent may exit before the child has closed its sockets.
            deadline = time.monotonic() + 1
            while True:
                try:
                    probe.bind(address)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.01)
        output, _ = proc.communicate(timeout=8)
        assert b'(final snapshot)' in output and b'Finished:' in output, output
        assert b'Log:' not in output, output
        assert not list(cache.glob('aot-save-*.log'))
        assert (cache / 'result-2').read_text() == '2\n'
        assert (cache / 'result-1').exists() != failure
    for failure in (False, True):
        cache = work / ('sync-' + str(failure))
        cache.mkdir()
        start = time.monotonic()
        proc = subprocess.Popen(
            [str(binary), str(cache) + '/',
             'fail' if failure else 'ok', 'sync'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            proc.wait(timeout=1)
            raise AssertionError('synchronous shutdown returned before save')
        except subprocess.TimeoutExpired:
            pass
        output, _ = proc.communicate(timeout=8)
        assert proc.returncode == 0, output
        assert time.monotonic() - start >= 3
        assert (cache / 'result-2').read_text() == '2\n'
        assert (cache / 'result-1').exists() != failure
        assert b'finished, status=' in output, output
print('PASS: parent exit and socket release precede slow save; background '
      'snapshots log to stderr without log files, including prior failure')
