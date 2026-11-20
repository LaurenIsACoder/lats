#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise production AOT discovery with a synthetic control-flow graph."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
start = source.index("typedef struct AOTTUKey")
end = source.index("static int aot_full_pc_cmp(", start)
production = source[start:end]
harness = r"""
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#define TARGET_FMT_lx "%lx"
#define TARGET_PAGE_SIZE 4096
#define TARGET_PAGE_MASK (~(uint64_t)4095)
#define MAX_TB_IN_TU 3
#define MAX_TB_IN_AOT_TU MAX_TB_IN_TU
#define TU_TB_INDEX_NEXT 0
#define TU_TB_INDEX_TARGET 1
#define TU_TRANSLATE_OK 0
#define lsassert assert
typedef uint64_t target_ulong;
typedef int CPUState;
typedef struct TranslationBlock TranslationBlock;
struct separated_data {
    int last_ir1_type;
    target_ulong x86_next_pc, x86_target_pc;
    TranslationBlock *next_tb[2];
    int tu_jmp[2];
};
struct TranslationBlock {
    target_ulong pc, cs_base;
    uint32_t flags, cflags;
    struct { void *ptr; } tc;
    struct separated_data *s_data;
};
enum { IR1_TYPE_BRANCH, IR1_TYPE_JUMP, IR1_TYPE_CALLIN, IR1_TYPE_CALL,
       IR1_TYPE_NORMAL, IR1_TYPE_SYSCALL, IR1_TYPE_JUMPIN, IR1_TYPE_RET,
       IR1_TYPE_EOB };
static struct {
    unsigned tb_num, ir1_num_in_tu;
    TranslationBlock *tb_list[MAX_TB_IN_TU];
} data, *tu_data = &data;
static int creates, fail_translation;
static int option_aot_tu_pretranslate;
static uint64_t latxs_ghbr_stats_checkpoint(void) { return 0; }
static void latxs_ghbr_stats_restore(uint64_t checkpoint)
{ (void)checkpoint; }
static void latxs_ghbr_stats_commit(void) {}
static target_ulong get_page(target_ulong pc) { return pc & TARGET_PAGE_MASK; }
static bool inst_may_cross_page(target_ulong pc)
{ return get_page(pc) != get_page(pc + 14); }
static int get_max_insns(int flags) { return 100; }
static void tu_push_back(TranslationBlock *tb)
{ assert(data.tb_num < MAX_TB_IN_TU); data.tb_list[data.tb_num++] = tb; }
static TranslationBlock *aot_tb_create(CPUState *cpu, target_ulong pc,
        uintptr_t hva, TranslationBlock *from, int limit, bool recorded)
{
    creates++;
    if (pc == 0x1400) { return NULL; }
    TranslationBlock *tb = g_malloc0(sizeof(*tb) +
                                     sizeof(struct separated_data));
    *tb = *from;
    tb->pc = pc;
    tb->tc.ptr = NULL;
    tb->s_data = (void *)(tb + 1);
    tb->s_data->last_ir1_type = IR1_TYPE_RET;
    if (pc == 0x1100) {
        tb->s_data->last_ir1_type = IR1_TYPE_BRANCH;
        tb->s_data->x86_next_pc = 0x1120;
        tb->s_data->x86_target_pc = 0x1140;
    } else if (pc == 0x1120 || pc == 0x1180) {
        tb->s_data->last_ir1_type = IR1_TYPE_JUMP;
        tb->s_data->x86_target_pc = 0x1100;
    }
    return tb;
}
static int tb_sort_cmp(const void *a, const void *b)
{
    const TranslationBlock *x = *(TranslationBlock **)a;
    const TranslationBlock *y = *(TranslationBlock **)b;
    return (x->pc > y->pc) - (x->pc < y->pc);
}
static void solve_tb_overlap(unsigned n, TranslationBlock **tb, int max) {}
static void tu_optimize_ir1(CPUState *cpu, TranslationBlock **tb, unsigned n,
                            target_ulong cs, uint32_t f, int cf) {}
static int translate_tu(CPUState *cpu, unsigned n, TranslationBlock **tb)
{
    assert(n && n <= MAX_TB_IN_TU);
    for (unsigned i = 0; i < n; i++) { tb[i]->tc.ptr = (void *)1; }
    return fail_translation ? 1 : TU_TRANSLATE_OK;
}
"""
harness += production
harness += r"""
static void release(TranslationBlock **tb, int n)
{
    for (int i = 0; i < n; i++) { g_free(tb[i]); }
    g_free(tb);
}
int main(void)
{
    CPUState cpu = 0;
    TranslationBlock seed = { .pc = 0x1100, .cs_base = 7,
                             .flags = 5, .cflags = 0x80000000 };
    TranslationBlock *records[] = { &seed };
    int count = 1;
    TranslationBlock **ans = retranslate_by_tu(&cpu, 0x1000, 0x8000,
                                               records, &count);
    assert(ans && count == 1 && creates == 1);
    assert(ans[0]->pc == 0x1100);
    release(ans, count);

    /* Explicitly enable discovery of TBs absent from the JIT-TB records. */
    option_aot_tu_pretranslate = 1;
    creates = 0;
    count = 1;
    ans = retranslate_by_tu(&cpu, 0x1000, 0x8000,
                                               records, &count);
    assert(ans && count == 3 && creates == 3); /* output grows past input */
    assert(ans[0]->pc == 0x1100 && ans[1]->pc == 0x1120);
    assert(ans[1]->cflags == seed.cflags && ans[1]->cs_base == seed.cs_base);
    assert(ans[0]->s_data->next_tb[0] == ans[1]);
    release(ans, count);

    /* A later TU must dispatch to an earlier TU, never bake in its address. */
    TranslationBlock tail = seed;
    tail.pc = 0x1180;
    TranslationBlock *two_tus[] = { &seed, &tail };
    count = 2;
    ans = retranslate_by_tu(&cpu, 0x1000, 0x8000, two_tus, &count);
    assert(ans && count == 4);
    assert(ans[3]->pc == 0x1180 && !ans[3]->s_data->next_tb[1]);
    release(ans, count);

    /* Same PC with a different complete translation key is not deduplicated. */
    TranslationBlock other = seed, flag_mode = seed, cflag_mode = seed;
    other.cs_base++;
    flag_mode.flags ^= 0x80000000;
    cflag_mode.cflags ^= 0x40000000;
    TranslationBlock *modes[] = { &seed, &other, &flag_mode, &cflag_mode };
    count = 4;
    ans = retranslate_by_tu(&cpu, 0x1000, 0x8000, modes, &count);
    assert(ans && count == 12);
    assert(ans[0]->cs_base != ans[3]->cs_base);
    assert(ans[0]->flags != ans[6]->flags);
    assert(ans[0]->cflags != ans[9]->cflags);
    release(ans, count);

    AOTTUPage page = {
        .records = g_hash_table_new_full(aot_tu_key_hash, aot_tu_key_equal,
                                         g_free, NULL),
        .decoded = g_hash_table_new_full(aot_tu_key_hash, aot_tu_key_equal,
                                         g_free, NULL),
        .blocks = g_ptr_array_new(), .pc = 0x1000, .hva = 0x8000,
    };
    data.tb_num = 0;
    int old = creates;
    assert(!aot_tu_successor(&page, &cpu, &seed, 0x2000));
    assert(!aot_tu_successor(&page, &cpu, &seed, 0x1fff));
    assert(creates == old);
    assert(!aot_tu_successor(&page, &cpu, &seed, 0x1400));
    assert(!aot_tu_successor(&page, &cpu, &seed, 0x1400));
    assert(creates == old + 1); /* failed decode is memoized */
    page.speculative = TARGET_PAGE_SIZE;
    assert(!aot_tu_successor(&page, &cpu, &seed, 0x1500));
    aot_tu_insert(page.records, aot_tu_key(&seed, 0x1500), &seed);
    TranslationBlock *tb = aot_tu_successor(&page, &cpu, &seed, 0x1500);
    assert(tb); /* budget must not discard observed TBs */
    assert(aot_tu_successor(&page, &cpu, &seed, 0x1500) == tb);
    tb->tc.ptr = (void *)1;
    assert(!aot_tu_successor(&page, &cpu, &seed, 0x1500));
    data.tb_num = MAX_TB_IN_TU;
    assert(!aot_tu_successor(&page, &cpu, &seed, 0x1600));
    g_hash_table_destroy(page.records);
    g_hash_table_destroy(page.decoded);
    release((TranslationBlock **)g_ptr_array_free(page.blocks, false), 1);

    count = 0;
    assert(!retranslate_by_tu(&cpu, 0x1000, 0x8000, records, &count));
    TranslationBlock stale = seed;
    stale.pc = 0x1400;
    TranslationBlock *stale_records[] = { &stale };
    count = 1;
    ans = retranslate_by_tu(&cpu, 0x1000, 0x8000, stale_records, &count);
    assert(ans && count == 0); /* no cacheable code is not a save failure */
    release(ans, count);

    fail_translation = 1;
    count = 1;
    assert(!retranslate_by_tu(&cpu, 0x1000, 0x8000, records, &count));
    assert(count == 0);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="aot-tu-pretranslate-") as tmp:
    src = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    src.write_text(harness)
    flags = subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu11", "-Wall", "-Werror", "-Wno-sign-compare",
                    str(src), "-o", str(exe)] + shlex.split(flags), check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: AOT successor pretranslation switch, keys, cycles, bounds, "
      "budgets and vector growth")
