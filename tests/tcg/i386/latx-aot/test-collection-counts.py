#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check the production QHT callback counts JIT entries before filtering."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()
start = source.index("static void do_tb_count(")
callback = source[start:source.index("\n}", start) + 2]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define IS_AOT_TB 1
#define HF_CPL_MASK 3
typedef struct { unsigned flags, bool_flags; bool reject; } TranslationBlock;
typedef struct { int jit_core_tb_num, jit_user_tb_num, tb_num, all_tb_num; }
    AOTSaveContext;
static bool not_save(TranslationBlock *tb)
{ return tb->reject || (tb->bool_flags & IS_AOT_TB); }
''' + callback + r'''
int main(void) {
    AOTSaveContext ctx = {0};
    TranslationBlock entries[] = {
        {0, 0, false}, {3, 0, false}, {0, 0, true}, {3, 0, true},
        {0, IS_AOT_TB, false}, {3, IS_AOT_TB, false}
    };
    for (int i = 0; i < 6; i++) do_tb_count(entries + i, 0, &ctx);
    assert(ctx.all_tb_num == 6 && ctx.tb_num == 2);
    assert(ctx.jit_core_tb_num == 2 && ctx.jit_user_tb_num == 2);
}
'''
with tempfile.TemporaryDirectory(prefix="aot-collection-") as directory:
    binary = str(Path(directory) / "test")
    subprocess.run(["cc", "-x", "c", "-", "-o", binary],
                   input=program, text=True, check=True)
    subprocess.run([binary], check=True)
print("PASS: resident JIT classification includes save-filtered TBs")
