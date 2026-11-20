#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise production relocation recording and actual TB flush callbacks."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
reloc = (root / "target/i386/latx/sbt/sys-aot-reloc.c").read_text()
maint = (root / "accel/tcg/tb-maint.c").read_text()
save = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


# Cleanup must not depend on whether a new snapshot could be started.
assert "clear_rel_table" not in function(save, "void aot_save_on_tb_flush(")

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#define CONFIG_SOFTMMU 1
#define HF_CPL_MASK 3
typedef struct TranslationBlock {
    uint32_t flags;
    int start, end;
} TranslationBlock;
#define TB_REL_START(tb) ((tb)->start)
#define TB_REL_END(tb) ((tb)->end)
typedef struct aot_rel {
    int kind;
    uint32_t tc_offset, rel_slots_num;
    uint64_t extra_addend;
    int next_entry_id;
} aot_rel;
static aot_rel *rel_table, curr_rel_table[1024];
static uint32_t rel_entry_num, rel_table_capacity;
static pthread_mutex_t rel_table_lock = PTHREAD_MUTEX_INITIALIZER;
static int option_sys_aot = 1, option_aot_save = 1;
static unsigned option_tu_mode;
#define LATX_TU_MODE_JIT (1u << 0)
#define LATX_TU_MODE_AOT (1u << 1)
static inline bool jit_tu_enabled(void)
{ return option_tu_mode & LATX_TU_MODE_JIT; }
static inline bool aot_tu_requested(void)
{ return option_tu_mode & LATX_TU_MODE_AOT; }
static inline bool aot_tu_enabled(void)
{ return option_sys_aot && aot_tu_requested(); }
typedef struct CPUEnv { void *syscall_tb; } CPUEnv;
typedef struct CPUState { CPUEnv *env_ptr; } CPUState;
typedef union run_on_cpu_data {
    unsigned host_int;
    uint64_t host_ulong;
} run_on_cpu_data;
static CPUEnv fixture_env;
static CPUState fixture_cpu = { &fixture_env };
static struct { unsigned tb_flush_count, tb_flush_count_mtcc[2];
                int n, htable[2]; } tb_ctx;
static unsigned save_requests, removed, plugins;
#define mmap_lock() ((void)0)
#define mmap_unlock() ((void)0)
#define CPU_FOREACH(cpu) for ((cpu) = &fixture_cpu; (cpu); (cpu) = NULL)
#define tcg_flush_jmp_cache(cpu) ((void)(cpu))
#define tu_generation_invalidate(cpu) ((void)(cpu))
#define tu_generation_reset_ids() ((void)0)
#define qht_reset_size(ht, size) ((void)(ht), (void)(size))
#define CODE_GEN_HTABLE_SIZE 1
#define flaged_shadow_htable_reset() ((void)0)
#define flaged_shadow_htable_reset_mtcc(id) ((void)(id))
#define tb_remove_all() (removed |= 3)
#define tb_remove_mtcc(id) (removed |= 1u << (id))
#define tcg_region_reset_all() ((void)0)
#define tcg_region_reset_mtcc(id) ((void)(id))
#define qatomic_inc(p) (++*(p))
#define qemu_plugin_flush_cb() (++plugins)
/*
 * Models success, an in-flight save, or failed fork without touching
 * the table.
 */
#define aot_save_on_tb_flush(id) ((void)(id), ++save_requests)
'''
for signature in ("void add_rel_entry(", "void clear_rel_table(",
                  "void aot_reloc_reset_on_tb_flush("):
    harness += function(reloc, signature) + "\n"
for signature in ("static void do_tb_flush(", "static void do_tb_flush_mtcc("):
    harness += function(maint, signature) + "\n"
harness += r'''
static TranslationBlock append(uint32_t flags)
{
    TranslationBlock tb = { flags, 0, 1 };
    add_rel_entry(&tb);
    return tb;
}
static void reset_fixture(void)
{
    free(rel_table);
    rel_table_capacity = 8;
    rel_table = calloc(rel_table_capacity, sizeof(*rel_table));
    assert(rel_table);
    rel_entry_num = 0;
    memset(&tb_ctx, 0, sizeof(tb_ctx));
    tb_ctx.n = 2;
    save_requests = removed = plugins = 0;
    curr_rel_table[0].kind = 11;
    curr_rel_table[1].extra_addend = 0x1234;
}
static void flush(unsigned id)
{
    do_tb_flush_mtcc(&fixture_cpu, (run_on_cpu_data) {
        .host_ulong = ((uint64_t)id << 32) | tb_ctx.tb_flush_count_mtcc[id]
    });
}
int main(void)
{
    for (int jit_tu = 0; jit_tu <= 1; jit_tu++) {
        for (int aot_tu = 0; aot_tu <= 1; aot_tu++) {
            option_tu_mode = (jit_tu ? LATX_TU_MODE_JIT : 0) |
                             (aot_tu ? LATX_TU_MODE_AOT : 0);
            reset_fixture();
            TranslationBlock user = append(HF_CPL_MASK);
#ifdef CONFIG_LATX_TU
            bool skip_user = aot_tu_requested();
#else
            bool skip_user = false;
#endif
            assert(rel_entry_num == (skip_user ? 0 : 2));
            assert(user.start == (skip_user ? -1 : 0));
            assert(user.end == (skip_user ? -1 : 1));
            TranslationBlock core = append(0);
            assert(core.start == (skip_user ? 0 : 2));
            assert(rel_table[core.start].kind == 11);
            assert(rel_table[core.end].extra_addend == 0x1234);
            uint32_t entries = rel_entry_num;
            flush(1);
            assert(rel_entry_num == entries);  /* live core references */
            flush(0);
            assert(rel_entry_num == (skip_user ? 0 : entries));
            assert(removed == 3 && plugins == 2 && save_requests == 2);
            do_tb_flush(&fixture_cpu, (run_on_cpu_data) { .host_int = 0 });
            assert(rel_entry_num == 0);  /* all users are gone */
        }
    }
#ifdef CONFIG_LATX_TU
    /* Repeated generations must reuse indices instead of growing the table. */
    option_tu_mode = LATX_TU_MODE_AOT;
    reset_fixture();
    for (int generation = 0; generation < 10000; generation++) {
        append(HF_CPL_MASK);
        assert(append(0).start == 0);
        flush(0);
        assert(rel_entry_num == 0 && rel_table_capacity == 8);
    }
    /* A stale flush request does not invalidate still-live TB indices. */
    append(0);
    do_tb_flush_mtcc(&fixture_cpu, (run_on_cpu_data) { .host_ulong = 0 });
    assert(rel_entry_num == 2);
    tb_ctx.tb_flush_count = 1;
    do_tb_flush(&fixture_cpu, (run_on_cpu_data) { .host_int = 0 });
    assert(rel_entry_num == 2);

    /* A real fork retains old entries while the parent's next epoch reuses
     * them. */
    int completion[2], status;
    assert(pipe(completion) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        char ready;
        close(completion[1]);
        assert(read(completion[0], &ready, 1) == 1);
        assert(rel_entry_num == 2 && rel_table[0].kind == 11);
        _exit(0);
    }
    close(completion[0]);
    flush(0);
    curr_rel_table[0].kind = 99;
    assert(append(0).start == 0 && rel_table[0].kind == 99);
    assert(write(completion[1], "x", 1) == 1);
    close(completion[1]);
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
    reset_fixture();
    option_aot_save = 0;
    append(0);
    assert(rel_entry_num == 0);
    free(rel_table);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="aot-reloc-lifetime-") as directory:
    path = Path(directory)
    cfile = path / "test.c"
    cfile.write_text(harness)
    for tu in (False, True):
        binary = path / ("test-tu" if tu else "test-no-tu")
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread",
            str(cfile), "-o", str(binary)]
        if tu:
            command.append("-DCONFIG_LATX_TU=1")
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)
print("PASS: user/core relocation ownership, flush epochs and fork isolation")
