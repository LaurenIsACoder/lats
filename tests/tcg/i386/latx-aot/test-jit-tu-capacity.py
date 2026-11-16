#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Execute production JIT queue discovery with bounded fake TB allocation."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'target/i386/latx/optimization/sys-tu.c').read_text()

def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define CONFIG_SOFTMMU 1
#define g_newa(type, n) ((type *)__builtin_alloca((n) * sizeof(type)))
static size_t live_heap_tables;
static __attribute__((unused)) void *tracked_calloc(size_t n, size_t size)
{ live_heap_tables++; return calloc(n, size); }
static __attribute__((unused)) void tracked_free(void *p)
{ if (*(void **)p) { live_heap_tables--; free(*(void **)p); } }
#define g_autofree __attribute__((cleanup(tracked_free)))
#define g_new0(type, n) ((type *)tracked_calloc((n), sizeof(type)))
static jmp_buf explore_exit;
static int force_exit;
#define CF_INVALID 1u
#define IS_AOT_TB 8u
typedef uint64_t ADDRX;
typedef uint64_t target_ulong;
typedef struct { int unused; } CPUState;
enum { IR1_TYPE_BRANCH, IR1_TYPE_JUMP, IR1_TYPE_CALLIN, IR1_TYPE_CALL,
       IR1_TYPE_NORMAL, IR1_TYPE_SYSCALL, IR1_TYPE_JUMPIN, IR1_TYPE_RET,
       IR1_TYPE_EOB, TU_TB_START_NORMAL, TU_TB_START_JMP };
enum { TU_EXPLORE_OK, TU_TB_INDEX_NEXT = 0, TU_TB_INDEX_TARGET = 1 };
typedef struct TranslationBlock TranslationBlock;
typedef struct TranslationUnitPages {
    TranslationBlock **hras_members;
    uint32_t hras_member_count;
} TranslationUnitPages;
struct separated_data {
    uint64_t x86_next_pc, x86_target_pc;
    int last_ir1_type, tu_jmp[2];
    TranslationBlock *next_tb[2];
};
struct TranslationBlock {
    uint64_t pc;
    uint32_t ir2_num, cflags, bool_flags;
    TranslationUnitPages *owner;
    struct { void *ptr; } tc;
    struct separated_data *s_data;
};
static struct {
    uint32_t tb_num;
    TranslationBlock *tb_list[1025];
} storage, *tu_data = &storage;
static TranslationBlock blocks[1026];
static struct separated_data data[1026];
static TranslationBlock cached[4];
static struct separated_data cached_data[4];
static unsigned cached_count;
static TranslationBlock *cached_members[4];
static TranslationUnitPages owners[2];
static int cross, limit, allocated, reject_page, failure;
#define TB_TU_PAGES(tb) ((tb)->owner)
static bool tb_is_tu(TranslationBlock *tb) { return tb->owner != NULL; }
static unsigned tb_cflags(TranslationBlock *tb) { return tb->cflags; }
static uintptr_t tb_tu_identity(TranslationBlock *tb) { return (uintptr_t)tb->owner; }
static int call_first, lookup_enabled;
static int option_tu_hras, option_rastoptp, option_soft_ras, in_aot_translate;
static int option_tu_hras_opt_copy = 0;
#define HF_CS64_MASK (1u << 15)
#define HF_CPL_MASK 3u
#define MAX_TB_IN_TS 1024
#define MAX_TB_IN_TU (limit < 1024 ? limit : 1024)
#define lsassert assert
static int cp_tu_pages(void) { return cross; }
static int cp_tu(void) { return cross; }
static int cp_tu_gen_tb(void) { return 1; }
static int tu_cross_page_enabled(void) { return cross; }
static uint64_t get_page(uint64_t pc) { return pc >> 12; }
static void tu_pages_init(void) {}
static int tu_pages_append(uint64_t pc) { return !reject_page; }
static int page_valid(CPUState *cpu, uint64_t pc) { return 1; }
static int tu_pages_find(uint64_t pc) { return 0; }
static void tu_pages_tb_add(TranslationBlock *tb) {}
static TranslationBlock *tu_tree_lookup(uint64_t pc)
{
    if (lookup_enabled) {
        for (unsigned i = 0; i < tu_data->tb_num; i++) {
            if (tu_data->tb_list[i]->pc == pc) return tu_data->tb_list[i];
        }
    }
    return NULL;
}
static TranslationBlock *tb_htable_lookup_noexit(CPUState *cpu,
    uint64_t pc, uint64_t cs, uint32_t flags, int cf)
{
    for (unsigned i = 0; i < cached_count; i++)
        if (cached[i].pc == pc) return &cached[i];
    return NULL;
}
static TranslationBlock *tb_create(CPUState *cpu, uint64_t pc, uint64_t cs,
    uint32_t flags, int cf, int insns, int first, int mode,
    TranslationBlock *hras_caller, int *result)
{
    if (force_exit) longjmp(explore_exit, 1);
    *result = failure;
    if (failure) return NULL;
    allocated++;
    unsigned n = tu_data->tb_num;
    assert(n < 1024);
    blocks[n].pc = pc;
    blocks[n].s_data = &data[n];
    data[n].last_ir1_type = IR1_TYPE_RET;
    for (unsigned i = 0; i < cached_count; i++) {
        if (cached[i].pc == pc) {
            data[n].last_ir1_type = cached_data[i].last_ir1_type;
            data[n].x86_next_pc = cached_data[i].x86_next_pc;
            data[n].x86_target_pc = cached_data[i].x86_target_pc;
        }
    }
    if (call_first && allocated == 1) {
        data[n].last_ir1_type = IR1_TYPE_CALL;
        data[n].x86_next_pc = pc + 5;
    }
    return &blocks[n];
}
'''
main = r'''
static void run(int mode, int config, int initial, int kind,
                int blocked, int fail, int expected)
{
    cross = mode; limit = config; reject_page = blocked; failure = fail;
    allocated = 0;
    for (int i = 0; i < 1026; i++) {
        blocks[i] = (TranslationBlock){.pc = 0x1000, .s_data = &data[i]};
        data[i] = (struct separated_data){.last_ir1_type = IR1_TYPE_RET,
                                         .tu_jmp = {1, 1}};
    }
    for (int i = 0; i < initial; i++) storage.tb_list[i] = &blocks[i];
    storage.tb_num = initial;
    storage.tb_list[1024] = &blocks[1025];
    data[0].last_ir1_type = kind;
    data[0].x86_next_pc = 0x1004;
    data[0].x86_target_pc = mode && kind == IR1_TYPE_BRANCH ? 0x2004 : 0x1008;
    CPUState cpu;
    assert(explore_queue(&cpu, 0, 0, 0, 0) == fail);
    assert(allocated == expected);
    assert(storage.tb_num == initial + expected);
    assert(storage.tb_list[1024] == &blocks[1025]);
    if (kind == IR1_TYPE_BRANCH && expected == 1) {
        assert(data[0].next_tb[0] == &blocks[initial]);
        assert(data[0].next_tb[1] == NULL);
    }
}
static void run_hras_priority(void)
{
    option_tu_hras = 2;
    option_rastoptp = option_soft_ras = 1;
    cross = 1; limit = 128; reject_page = failure = allocated = 0;
    call_first = 1;
    for (unsigned i = 0; i < 1026; i++) {
        blocks[i] = (TranslationBlock){.pc = 0x1000, .s_data = &data[i]};
        data[i] = (struct separated_data){.last_ir1_type = IR1_TYPE_RET,
                                         .tu_jmp = {1, 1}};
        if (i < 1024) storage.tb_list[i] = &blocks[i];
    }
    storage.tb_num = 1022;
    data[0].last_ir1_type = IR1_TYPE_BRANCH;
    data[0].x86_next_pc = 0x1004;
    data[0].x86_target_pc = 0x1008;
    CPUState cpu;
    assert(explore_queue(&cpu, 0, HF_CS64_MASK | HF_CPL_MASK, 0, 0) == 0);
    assert(storage.tb_num == 1024 && allocated == 2);
    assert(data[0].next_tb[0] == &blocks[1022]);
    assert(data[1022].next_tb[0] == &blocks[1023]);
    assert(blocks[1023].pc == 0x1009);
    assert(data[0].next_tb[1] == NULL);

    /* A full queue must still resolve a CALL to its existing local return. */
    call_first = 0; lookup_enabled = 1; allocated = 0;
    data[0].last_ir1_type = IR1_TYPE_CALLIN;
    data[0].next_tb[0] = NULL;
    assert(explore_queue(&cpu, 0, HF_CS64_MASK | HF_CPL_MASK, 0, 0) == 0);
    assert(storage.tb_num == 1024 && allocated == 0);
    assert(data[0].next_tb[0] == &blocks[1022]);
}
static void run_hras_cached_return(int initial, bool blocked, bool loop)
{
    option_tu_hras = 2; option_rastoptp = option_soft_ras = 1;
    cross = lookup_enabled = 1; call_first = failure = allocated = 0;
    reject_page = blocked;
    for (unsigned i = 0; i < 1024; i++) {
        blocks[i] = (TranslationBlock){.pc = 0x1000, .s_data = &data[i]};
        data[i] = (struct separated_data){.last_ir1_type = IR1_TYPE_RET,
                                         .tu_jmp = {1, 1}};
        storage.tb_list[i] = &blocks[i];
    }
    storage.tb_num = initial;
    data[0].last_ir1_type = IR1_TYPE_CALL;
    data[0].x86_next_pc = 0x1005;
    const uint64_t pcs[] = {0x1005, 0x1009, 0x1010, 0x1018};
    cached_count = 4;
    for (unsigned i = 0; i < cached_count; i++) {
        cached[i] = (TranslationBlock){.pc = pcs[i], .owner = &owners[i == 3],
            .s_data = &cached_data[i], .tc.ptr = &cached[i]};
        cached_data[i] = (struct separated_data){.last_ir1_type = IR1_TYPE_RET};
        cached_members[i] = &cached[i];
    }
    owners[0] = (TranslationUnitPages){cached_members, 3};
    owners[1] = (TranslationUnitPages){cached_members + 3, 1};
    cached_data[0].last_ir1_type = IR1_TYPE_BRANCH;
    cached_data[0].x86_next_pc = pcs[1];
    cached_data[0].x86_target_pc = pcs[2];
    cached_data[2].last_ir1_type = IR1_TYPE_JUMP;
    cached_data[2].x86_target_pc = loop ? pcs[0] : pcs[3];
    CPUState cpu;
    assert(explore_queue(&cpu, 0, HF_CS64_MASK | HF_CPL_MASK, 0, 0) == 0);
    if (initial == 1 && !blocked && !option_tu_hras_opt_copy) {
        assert(allocated == 1 && storage.tb_num == initial + 1);
        assert(data[1].next_tb[0] == &cached[1]);
        assert(data[1].next_tb[1] == &cached[2]);
    } else if (initial == 1 && !blocked) {
        /* Both former intra-owner edges must be local; stop at another owner. */
        assert(allocated == 3 && storage.tb_num == 4);
        assert(data[1].next_tb[0] == &blocks[2]);
        assert(data[1].next_tb[1] == &blocks[3]);
        assert(data[3].next_tb[1] == (loop ? &blocks[1] : &cached[3]));
    } else {
        /* Rejected copies retain the valid cached targets, not NULL edges. */
        assert(allocated == 1 && storage.tb_num == initial + 1);
        assert(data[initial].next_tb[0] == &cached[1]);
        assert(data[initial].next_tb[1] == &cached[2]);
    }
    cached_count = 0;
}
static void run_hras_allocation_exit(void)
{
    force_exit = 1;
    for (volatile unsigned i = 0; i < 32; i++) {
        if (setjmp(explore_exit) == 0) {
            run_hras_cached_return(1, false, false);
            abort();
        }
        assert(live_heap_tables == 0);
    }
    force_exit = 0;
    cached_count = 0;
}
int main(void)
{
    CPUState cpu;
    /* The last member includes deferred exits: 105608 bytes was observed
     * with 1024 inline indirect CALLs, overflowing TBMini's size field. */
    for (int mode = 0; mode <= 2; mode++) {
        option_tu_hras = mode;
        assert(tu_hras_member_size_fits(16383));
        assert(tu_hras_member_size_fits(16384) == !mode);
        assert(tu_hras_member_size_fits(105608 / 4) == !mode);
    }
    TranslationBlock *members[] = { &blocks[0], &blocks[1], &blocks[2] };
    blocks[0].ir2_num = blocks[1].ir2_num = 16383;
    blocks[2].ir2_num = 105608 / 4;
    option_tu_hras = 0;
    assert(check_member_sizes(3, members) == 0 && size_checks == 0);
    for (int mode = 1; mode <= 2; mode++) {
        option_tu_hras = mode;
        assert(check_member_sizes(3, members) == TU_TRANSLATE_FALLBACK);
        assert(size_checks == 3 * mode && finished == mode);
    }
    blocks[2].ir2_num = 16383;
    assert(check_member_sizes(3, members) == 0 && size_checks == 9);
    option_tu_hras = 0;
    for (int kind = IR1_TYPE_BRANCH; kind <= IR1_TYPE_SYSCALL; kind++) {
        int successors = kind == IR1_TYPE_BRANCH ? 2 : 1;
        run(1, 128, 128, kind, 0, 0, successors);
        run(1, 128, 1023, kind, 0, 0, 1);
        run(1, 128, 1024, kind, 0, 0, 0);
        run(0, 128, 127, kind, 0, 0, 1);
        run(0, 128, 128, kind, 0, 0, 0);
        run(0, 2048, 1023, kind, 0, 0, 1);
        run(1, 128, 127, kind, 1, 0, 0);
        run(1, 128, 127, kind, 0, 7, 0);
    }
    storage.tb_num = 1024;
    tu_push_back(&blocks[1024]);
    assert(storage.tb_num == 1024);
    assert(storage.tb_list[1024] == &blocks[1025]);
    /* Only the original complete HRAS condition enters the 8 KiB wrapper. */
    storage.tb_num = 0;
    for (int disabled = 0; disabled < 8; disabled++) {
        option_tu_hras = disabled == 1 ? 0 : 2;
        option_rastoptp = disabled != 2;
        option_soft_ras = disabled != 3;
        in_aot_translate = disabled == 4;
        uint32_t flags = (disabled == 5 ? 0 : HF_CS64_MASK) |
                         (disabled == 6 ? 0 : HF_CPL_MASK);
        unsigned before = hras_frames;
        assert(explore_queue(&cpu, disabled == 7, flags, 0, 0) == 0);
        assert(hras_frames == before + (disabled == 0));
    }
    in_aot_translate = 0;
    run_hras_cached_return(1, false, false);
    option_tu_hras_opt_copy = 1;
    run_hras_priority();
    run_hras_cached_return(1, false, false);
    run_hras_cached_return(1, false, true);
    run_hras_cached_return(1023, false, false);
    run_hras_cached_return(1, true, false);
    run_hras_allocation_exit();
    return 0;
}
'''
member_check = r'''
#define TU_TRANSLATE_FALLBACK 7
static unsigned size_checks, finished;
static void latxs_tr_fini(void) { finished++; }
static bool checked_member_size(uint32_t n)
{ size_checks++; return tu_hras_member_size_fits(n); }
#define tu_hras_member_size_fits checked_member_size
static int check_member_sizes(uint32_t tb_num_in_tu, TranslationBlock **tb_list)
{
'''
start = source.index('#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)', source.index(
    '    if (code_nr >= 0 && !tu_hras_local_branches_fit(code_nr))'))
start = source.index('\n', start) + 1
member_check += source[start:source.index('\n#endif', start)]
member_check += '\n    return 0;\n}\n#undef tu_hras_member_size_fits\n'

# Compile the actual caller dispatch as well as the HRAS-only stack owner.
wrapper = function('static __attribute__((noinline)) int get_tu_queue_hras(')
wrapper = wrapper.replace('    TranslationBlock *originals[MAX_TB_IN_TS];',
    '    TranslationBlock *originals[MAX_TB_IN_TS];\n    hras_frames++;')
start = source.index('    /* search all tbs we can get */')
dispatch = r'''
static int explore_queue(CPUState *cpu, target_ulong cs_base, uint32_t flags,
                         int cflags, int max_insns)
{
    int value, *result = &value;
''' + source[start:source.index('    if (*result != TU_EXPLORE_OK)', start)]
dispatch += '    return *result;\n}\n'

code = (prefix + function('inline void tu_push_back(').replace(
    'inline void', 'static void', 1) + '\n' +
    function('static bool tu_hras_member_size_fits(') + '\n' + member_check +
    function('static TranslationBlock *tu_hras_original_member(') + '\n' +
    function('static int tu_hras_complete_returns(') + '\n' +
    function('static inline int get_tu_queue(') + '\nstatic unsigned hras_frames;\n' +
    wrapper + dispatch + main)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2',
                    '-Wall', '-Werror', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: JIT capacity, HRAS return priority, cross-page policy and failures')
