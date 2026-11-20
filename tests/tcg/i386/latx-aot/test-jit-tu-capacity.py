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
typedef uint64_t ADDRX;
typedef uint64_t target_ulong;
typedef struct { int unused; } CPUState;
enum { IR1_TYPE_BRANCH, IR1_TYPE_JUMP, IR1_TYPE_CALLIN, IR1_TYPE_CALL,
       IR1_TYPE_NORMAL, IR1_TYPE_SYSCALL, IR1_TYPE_JUMPIN, IR1_TYPE_RET,
       IR1_TYPE_EOB, TU_TB_START_NORMAL, TU_TB_START_JMP };
enum { TU_EXPLORE_OK, TU_TB_INDEX_NEXT = 0, TU_TB_INDEX_TARGET = 1 };
typedef struct TranslationBlock TranslationBlock;
struct separated_data {
    uint64_t x86_next_pc, x86_target_pc;
    int last_ir1_type, tu_jmp[2];
    TranslationBlock *next_tb[2];
};
struct TranslationBlock {
    uint64_t pc;
    struct { void *ptr; } tc;
    struct separated_data *s_data;
};
static struct {
    uint32_t tb_num;
    TranslationBlock *tb_list[1025];
} storage, *tu_data = &storage;
static TranslationBlock blocks[1026];
static struct separated_data data[1026];
static int cross, limit, allocated, reject_page, failure;
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
static TranslationBlock *tu_tree_lookup(uint64_t pc) { return NULL; }
static TranslationBlock *tb_htable_lookup_noexit(CPUState *cpu,
    uint64_t pc, uint64_t cs, uint32_t flags, int cf) { return NULL; }
static TranslationBlock *tb_create(CPUState *cpu, uint64_t pc, uint64_t cs,
    uint32_t flags, int cf, int insns, int first, int mode, int *result)
{
    *result = failure;
    if (failure) return NULL;
    allocated++;
    unsigned n = tu_data->tb_num;
    assert(n < 1024);
    blocks[n].pc = pc;
    blocks[n].s_data = &data[n];
    data[n].last_ir1_type = IR1_TYPE_RET;
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
    assert(get_tu_queue(&cpu, 0, 0, 0, 0) == fail);
    assert(allocated == expected);
    assert(storage.tb_num == initial + expected);
    assert(storage.tb_list[1024] == &blocks[1025]);
    if (kind == IR1_TYPE_BRANCH && expected == 1) {
        assert(data[0].next_tb[0] == &blocks[initial]);
        assert(data[0].next_tb[1] == NULL);
    }
}
int main(void)
{
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
    return 0;
}
'''
code = (prefix + function('inline void tu_push_back(').replace(
    'inline void', 'static void', 1) + '\n' +
    function('static inline int get_tu_queue(') + main)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2',
                    '-Wall', '-Werror', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: production JIT queue capacity, cross-page policy and failures')
