#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
# SPDX-License-Identifier: GPL-2.0-only

"""Check host-PC attribution of JIT CALL repair code in the region tree."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).resolve().parents[4]
tree = (root / 'tcg/region-tree.inc').read_text()
header = (root / 'include/exec/translation-block.h').read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2] + '\n'


flags = '\n'.join(line for line in header.splitlines()
                  if line.startswith(('#define IS_AOT_TB ', '#define IS_TU_TB ')))

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define CONFIG_LATX_TU 1
''' + flags + r'''

struct tb_tc {
    const void *ptr;
    size_t size;
    size_t offset_in_tu;
};
typedef struct TranslationBlock TranslationBlock;
typedef struct {
    TranslationBlock *next_tb[2];
    unsigned ras_tp_cold_offset;
    unsigned ras_tp_cold_insns;
} SideData;
struct TranslationBlock {
    struct tb_tc tc;
    unsigned bool_flags;
    SideData *s_data;
};
struct tcg_region_tree {
    int lock;
    void *tree;
};

static unsigned char code[80];
static SideData data[3];
static TranslationBlock members[3], native;
static struct tcg_region_tree region;
static bool has_native_body;

static struct tcg_region_tree *tc_ptr_to_region_tree(const void *ptr)
{
    (void)ptr;
    return &region;
}
static void qemu_mutex_lock_1(int *lock) { assert(!*lock); *lock = 1; }
static void qemu_mutex_unlock_1(int *lock) { assert(*lock); *lock = 0; }
static TranslationBlock *tcg_native_lookup(void *tree, uintptr_t pc)
{
    (void)tree;
    assert(region.lock);
    return has_native_body && pc == (uintptr_t)(code + 72) ? &native : NULL;
}
''' + function(tree, 'static int ptr_cmp_tb_tc(') + r'''
static TranslationBlock *q_tree_lookup(void *tree, const struct tb_tc *query)
{
    (void)tree;
    assert(region.lock);
    for (unsigned i = 0; i < 3; i++) {
        if (members[i].tc.size && !ptr_cmp_tb_tc(query->ptr, &members[i].tc)) {
            return &members[i];
        }
    }
    return NULL;
}
'''

signature = 'static TranslationBlock *tcg_jit_repair_owner('
if signature in tree:
    fixture += function(header, 'static inline bool tb_is_tu(')
    fixture += function(header, 'static inline uintptr_t tb_tu_identity(')
    fixture += function(tree, signature)
fixture += function(tree, 'TranslationBlock *tcg_tb_lookup(') + r'''
static void expect_owner(unsigned offset, const TranslationBlock *expected)
{
    assert(tcg_tb_lookup((uintptr_t)(code + offset)) == expected);
    assert(!region.lock);
}

int main(void)
{
    unsigned begin[] = {0, 8, 16};
    unsigned size[] = {8, 8, 52};
    for (unsigned i = 0; i < 3; i++) {
        data[i].next_tb[0] = i ? &members[i - 1] : NULL;
        data[i].next_tb[1] = i < 2 ? &members[i + 1] : NULL;
        members[i] = (TranslationBlock){
            .tc = {code + begin[i], size[i], begin[i]},
            .bool_flags = IS_TU_TB, .s_data = &data[i],
        };
    }
    data[0].ras_tp_cold_offset = 24;
    data[0].ras_tp_cold_insns = 6;
    data[1].ras_tp_cold_offset = 48;
    data[1].ras_tp_cold_insns = 4;

    expect_owner(4, &members[0]);
    expect_owner(12, &members[1]);
    expect_owner(20, &members[2]);
    for (unsigned off = 24; off < 64; off++) {
        expect_owner(off, off < 48 ? &members[0] : &members[1]);
    }
    expect_owner(64, &members[2]); /* Inside the member, past both stubs. */
    expect_owner(68, NULL);        /* End of the executable range. */

    /* A fragment outside the last member cannot steal an ordinary PC. */
    data[1].ras_tp_cold_insns = 7;
    expect_owner(48, &members[2]);
    data[1].ras_tp_cold_insns = 0;
    expect_owner(48, &members[2]);
    data[1].ras_tp_cold_insns = 4;

    members[2].bool_flags |= IS_AOT_TB;
    expect_owner(24, &members[2]);
    has_native_body = true;
    expect_owner(72, &native);
    members[2].bool_flags = 0;
    members[2].s_data = NULL;
    expect_owner(24, &members[2]);

    puts("PASS: region tree attributes exact JIT repair ranges to their CALL owners");
}
'''

with tempfile.TemporaryDirectory(prefix='lats-region-tree-repair-') as tmp:
    src, exe = Path(tmp) / 'test.c', Path(tmp) / 'test'
    src.write_text(fixture)
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                    '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
