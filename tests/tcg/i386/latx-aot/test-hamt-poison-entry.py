#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""An empty member must not overwrite its neighbour's shared entry."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/hamt/hamt.c").read_text()
start = source.index("void hamt_poison_tb_entry(")
body = source[start:source.index("\n}", start) + 2]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define HF_CS64_MASK 1
#define HF_CPL_MASK 6
#define CF_PCREL 8
#define CF_NO_ACTIVE 16
#define HAMT_INVALID_TB_INSN 1
#define hamt_enabled() true
#define qatomic_set(p, v) (*(p) = (v))
typedef struct {
    struct { void *ptr; } tc;
    unsigned ir2_num, flags, cflags, bool_flags, cs_base;
} TranslationBlock;
static unsigned flushes;
static unsigned tb_cflags(const TranslationBlock *tb) { return tb->cflags; }
static void flush_idcache_range(uintptr_t a, uintptr_t b, unsigned size)
{ assert(a == b && size == 4); flushes++; }
''' + body + r'''
int main(void)
{
    uint32_t shared_entry = 0xabcdef;
    TranslationBlock empty = {.tc = {&shared_entry}, .flags = 7};
    TranslationBlock active = empty;
    active.ir2_num = 4;
    active.bool_flags = 0x48; /* TU and AOT member */
    hamt_poison_tb_entry(&empty);
    assert(shared_entry == 0xabcdef && flushes == 0);
    hamt_poison_tb_entry(&active);
    assert(shared_entry == 1 && flushes == 1);
    static const uint32_t file_template = 0x123456;
    active.tc.ptr = (void *)&file_template;
    active.cflags = CF_NO_ACTIVE;
    hamt_poison_tb_entry(&active);
    assert(file_template == 0x123456 && flushes == 1);
}
'''
with tempfile.TemporaryDirectory(prefix="hamt-poison-entry-") as directory:
    binary = Path(directory) / "test"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-o", str(binary), "-"], input=program,
                   text=True, check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: empty shared entries and inactive file templates are not poisoned")
