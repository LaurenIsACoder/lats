#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check production HRAS member queries, including same-PC external copies."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]


def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct TranslationBlock { uint64_t pc; } TranslationBlock;
static int option_tu_hras, in_aot_translate;
static struct {
    TranslationBlock **tb_list;
    uint32_t tb_num;
} storage, *tu_data = &storage;
'''
code += function('target/i386/latx/include/latx-tu.h',
                 'static inline int tu_sorted_tb_index(')
code += function('target/i386/latx/optimization/sys-shbr.c',
                 'static int shbr_tb_index(')
for name in ('tb_is_in_tb_list', 'tb_is_adjacent'):
    code += function('target/i386/latx/optimization/sys-tu.c', 'bool ' + name + '(')
code += r'''
int main(void)
{
    static TranslationBlock blocks[1024], external;
    static TranslationBlock *list[1024];

    storage.tb_list = list;
    for (unsigned i = 0; i < 1024; i++) {
        blocks[i].pc = 0x400000 + 4 * i;
        list[i] = &blocks[i];
    }
    for (option_tu_hras = 1; option_tu_hras <= 2; option_tu_hras++) {
        for (unsigned count = 1; count <= 1024; count++) {
            storage.tb_num = count;
            assert(shbr_tb_index(count, list, NULL) == -1);
            assert(!tb_is_in_tb_list(NULL));
            for (unsigned i = 0; i < count; i++) {
                external.pc = blocks[i].pc;
                assert(tu_sorted_tb_index(count, list, &blocks[i]) == i);
                assert(shbr_tb_index(count, list, &blocks[i]) == i);
                assert(tb_is_in_tb_list(&blocks[i]));
                assert(!tb_is_in_tb_list(&external));
                assert(tu_sorted_tb_index(count, list, &external) == -1);
                assert(shbr_tb_index(count, list, &external) == -1);
                assert(!tb_is_adjacent(&external, list[0]));
                assert(tb_is_adjacent(&blocks[i], &blocks[i + (i + 1 < count)])
                       == (i + 1 < count));
            }
        }
        assert(tu_sorted_tb_index(0, list, list[0]) == -1);
        /* OFF and AOT keep their existing linear query on arbitrary lists. */
        for (in_aot_translate = 0; in_aot_translate <= 1; in_aot_translate++) {
            int saved = option_tu_hras;
            if (!in_aot_translate) option_tu_hras = 0;
            storage.tb_num = 3;
            list[0] = &blocks[2]; list[1] = &blocks[0]; list[2] = &blocks[1];
            assert(shbr_tb_index(3, list, &blocks[0]) == 1);
            assert(tb_is_in_tb_list(&blocks[1]));
            assert(tb_is_adjacent(&blocks[2], &blocks[0]));
            assert(!tb_is_adjacent(&blocks[0], &blocks[2]));
            option_tu_hras = saved;
        }
        in_aot_translate = 0;
        for (unsigned i = 0; i < 1024; i++) list[i] = &blocks[i];
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(code)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2',
                    '-Wall', '-Werror', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: HRAS sorted member identity and adjacency; OFF/AOT linear queries')
