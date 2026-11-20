#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check that shutdown saves synchronously after a failed fork."""
from pathlib import Path
import subprocess
import tempfile

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
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>

typedef struct {
    int core_tb_num;
    int user_tb_num;
    int tb_num;
} AOTSaveContext;
typedef struct {
    int mtcc_id;
    bool has_core;
    bool has_user;
} AOTFlushSaveTrigger;
typedef struct {
    AOTSaveContext *ctx;
    AOTFlushSaveTrigger trigger;
    bool merge_only;
    bool shutdown;
    int completion_read_fd;
    int completion_write_fd;
    int previous_save_fd;
} AOTFlushSaveJob;
typedef struct {
    int completion_fd;
} AOTRuntimeSaveState;

#define AOT_CORE 1
#define AOT_USER 2
#define AOT_SYNC_SAVE_MIN_TB 1000
#define qatomic_set(p, v) (*(p) = (v))

static bool waited;
static bool drained;
static bool translating;
static AOTSaveContext context;
static AOTRuntimeSaveState runtime = { .completion_fd = -1 };
static char *aot_file_path;
static void *offline_cpu = (void *)1, *global_offline_cpu = (void *)1;
static void *lsenv = (void *)1, *global_lsenv = (void *)1;
static void *tu_data = (void *)1, *global_tu_data = (void *)1;
static void *aot_tu_merge_cpu;
static int aot_merge_shutting_down;
static int in_aot_translate;
static int option_wait_aot;

static AOTSaveContext *aot_save_state_context(void) { return &context; }
static AOTRuntimeSaveState *aot_save_state_runtime(void) { return &runtime; }
static void aot_verbose_log(const char *fmt, ...) { (void)fmt; }
static void aot_merger_stop(void) { }
static void aot_save_capture_runtime_context(void) { }
static bool aot_poll_save_child(void) { return false; }
static bool aot_flush_save_trigger_prepare(void) { return true; }
static pid_t aot_start_save_child(AOTFlushSaveJob *job)
{
    assert(job->shutdown);
    assert(job->previous_save_fd == -1);
    errno = ENOMEM;
    return -1;
}
static void aot_wait_save_child(void)
{
    assert(!translating);
    waited = true;
}
static bool get_tb(AOTSaveContext *ctx, int mtcc_id, bool nonblocking)
{
    assert(waited);
    assert(translating);
    assert(offline_cpu == aot_tu_merge_cpu);
    assert(!nonblocking);
    assert(mtcc_id == -1);
    return true;
}
static void aot_save_context_begin_translate(void)
{
    assert(waited);
    translating = true;
    in_aot_translate = 1;
}
static void aot_save_context_end_translate(AOTSaveContext *ctx)
{
    assert(translating);
    translating = false;
    in_aot_translate = 0;
}
static void generate_aot_file(AOTSaveContext *ctx, const char *name,
                              uint32_t count, uint32_t mode)
{
    char path[PATH_MAX];
    FILE *file;

    assert(waited);
    assert(translating);
    assert(mode == AOT_CORE || mode == AOT_USER);
    snprintf(path, sizeof(path), "%s%s", aot_file_path, name);
    file = fopen(path, "w");
    assert(file);
    fprintf(file, "%u\n", count);
    fclose(file);
}
static void aot_merger_drain(void)
{
    assert(waited);
    assert(translating);
    drained = true;
}
'''

for signature in (
    'static bool aot_save_context_has_min_tb(AOTSaveContext *ctx, int min_tb_count)',
    'static void aot_save_run_sync(int mtcc_id)',
    'static void aot_flush_save_trigger_init(AOTFlushSaveTrigger *trigger,\n'
    '        int mtcc_id)',
    'void aot_save_on_shutdown(void)',
):
    program += function(signature) + '\n'

program += r'''
int main(int argc, char **argv)
{
    bool enough_tbs = argc > 2 && strcmp(argv[2], "save") == 0;

    assert(argc > 2);
    aot_file_path = argv[1];
    if (enough_tbs) {
        context.tb_num = 2100;
        context.core_tb_num = 1200;
        context.user_tb_num = 900;
    } else {
        context.tb_num = 999;
        context.user_tb_num = 999;
    }

    aot_save_on_shutdown();
    assert(waited);
    assert(drained);
    assert(!translating);
    assert(!in_aot_translate);
    assert(!aot_tu_merge_cpu);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='aot-fork-fallback-') as directory:
    work = Path(directory)
    binary = work / 'test'
    subprocess.run(
        ['cc', '-std=gnu11', '-Wall', '-Werror', '-x', 'c', '-o',
         str(binary), '-'],
        input=program, text=True, check=True)

    save_dir = work / 'save'
    save_dir.mkdir()
    output = subprocess.run(
        [str(binary), str(save_dir) + '/', 'save'], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True).stdout
    assert 'falling back to synchronous save' in output, output
    assert 'Synchronous save started' in output, output
    assert (save_dir / 'sys-aot-core').read_text() == '1200\n'
    assert (save_dir / 'sys-aot-user').read_text() == '900\n'

    drain_dir = work / 'drain'
    drain_dir.mkdir()
    output = subprocess.run(
        [str(binary), str(drain_dir) + '/', 'drain'], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True).stdout
    assert 'TB count 999 too few' in output, output
    assert not (drain_dir / 'sys-aot-core').exists()
    assert not (drain_dir / 'sys-aot-user').exists()

print('PASS: failed shutdown forks fall back to synchronous save with drain')
