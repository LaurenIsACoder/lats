#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Mixed compact/full TB relocations must preserve each address independently."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-reloc.c").read_text()
start = source.index("        case LOAD_TB_ADDR:")
body = source[start:source.index("#ifdef CONFIG_SOFTMMU", start)]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define LOAD_TB_ADDR 1
#define LOAD_TB_FULL_ADDR 2
#define lsassert assert
static bool split;
static bool split_tb_base_ok(void) { return split; }
static void relocate(uintptr_t address, const int *kinds, uint32_t *code, int count)
{
    void *tb = (void *)address;
    uintptr_t tb_addr = (uintptr_t)tb;
    (void)tb_addr;
    struct { int rel_slots_num; } entries[4] = {{3}, {3}, {3}, {3}};
    __typeof__(entries[0]) *aot_rel_table = entries;
    for (int i = 0; i < count; ++i) {
        uint32_t *pinsn = code + i * 3;
        int kind = kinds[i];
        switch (kind) {
''' + body + r'''
        default: assert(0);
        }
    }
}
int main(void)
{
    const int kinds[] = {LOAD_TB_ADDR, LOAD_TB_FULL_ADDR,
                         LOAD_TB_ADDR, LOAD_TB_FULL_ADDR};
    const uintptr_t addresses[] = {UINT64_C(0x555012345678), UINT64_C(0x7ffe7d9de940)};
    for (unsigned a = 0; a < 2; ++a) {
        for (unsigned mode = 0; mode < 2; ++mode) {
            uint32_t code[12];
            split = mode;
            for (unsigned i = 0; i < 4; ++i) {
                code[i*3] = 0x14000004;
                code[i*3+1] = 0x03800084;
                code[i*3+2] = 0x16000004;
            }
            relocate(addresses[a], kinds, code, 4);
            for (unsigned i = 0; i < 4; ++i) {
                uintptr_t decoded = ((uint64_t)((code[i*3] >> 5) & 0xfffff) << 12) |
                    ((code[i*3+1] >> 10) & 0xfff) |
                    ((uint64_t)((code[i*3+2] >> 5) & 0xfffff) << 32);
                uintptr_t expected = addresses[a];
                if (split && kinds[i] == LOAD_TB_ADDR) expected &= UINT32_MAX;
                assert(decoded == expected);
                assert((code[i*3] & 31) == 4 && (code[i*3+2] & 31) == 4);
            }
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix="ras-tp-reloc-") as directory:
    binary = Path(directory) / "test"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-o", str(binary), "-"], input=program,
                   text=True, check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: full TB relocation survives preceding compact addresses")
