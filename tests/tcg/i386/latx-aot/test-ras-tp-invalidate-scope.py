#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Kernel TB invalidation preserves TP RAS; user aliases and legacy still clear."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "accel/tcg/tb-maint.c").read_text()
start = source.index("static void tb_jmp_cache_inval_tb(")
body = source[start:source.index("\n}", start) + 2]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define HF_CPL_MASK 3
#define CF_PCREL 1
typedef struct CPUState { struct CPUState *next; unsigned flushes,lookups; } CPUState;
typedef struct { uint32_t flags,cflags; uint64_t pc; } TranslationBlock;
static CPUState second, first = {&second,0,0};
static int option_soft_ras = 1, option_rastoptp;
#define CPU_FOREACH(cpu) for(cpu=&first;cpu;cpu=cpu->next)
#define tb_cflags(tb) ((tb)->cflags)
static void latxs_flush_soft_ras(CPUState *cpu) { cpu->flushes++; }
static void tcg_flush_jmp_cache(CPUState *cpu) {
    cpu->lookups++;
    if(option_soft_ras && !option_rastoptp)latxs_flush_soft_ras(cpu);
}
static void tb_jmp_cache_invalidate(CPUState *cpu,uint64_t pc,TranslationBlock *tb) {
    assert(pc==tb->pc);cpu->lookups++;
}
''' + body + r'''
int main(void) {
    for(int tp=0;tp<2;tp++)for(int pcrel=0;pcrel<2;pcrel++) {
        option_rastoptp=tp;
        first.flushes=first.lookups=second.flushes=second.lookups=0;
        TranslationBlock kernel={0,pcrel,0x1000},user={3,pcrel,0x2000};
        tb_jmp_cache_inval_tb(&kernel);
        assert(first.lookups==1 && second.lookups==1);
        assert(first.flushes==!tp && second.flushes==!tp);
        /* A user alias is processed separately even after a kernel alias. */
        tb_jmp_cache_inval_tb(&user);
        assert(first.lookups==2 && second.lookups==2);
        assert(first.flushes==2U-(unsigned)tp && second.flushes==2U-(unsigned)tp);
    }
}
'''
with tempfile.TemporaryDirectory(prefix="ras-tp-invalidate-") as directory:
    binary = Path(directory) / "test"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-o", str(binary), "-"],
                   input=program, text=True, check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: kernel invalidations preserve TP RAS; user aliases and legacy clear it")
