#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check RAS code offsets using TB identity rather than the JIT TU switch."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]


def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define g_assert assert
#define qatomic_read(p) (*(p))
#define MF_RAS_CLEAR_B0 0x400
#define LATX_RAS_TP_CALL_SKIP (0x50000000U | (7U<<10))
#define IS_TU_TB 0x40
typedef struct {
    unsigned bool_flags, misc_flags;
    uintptr_t inst_ptr_offset;
    uint32_t ras_tp_cold_offset;
    struct { void *ptr; size_t offset_in_tu; } tc;
} TranslationBlock;
#define LATXS_RAS_TP_COLD_OFFSET(tb) ((tb)->ras_tp_cold_offset)
static unsigned option_tu_mode;
static int option_rastoptp, option_soft_ras = 1;
#define LATX_TU_MODE_JIT (1u << 0)
static inline bool jit_tu_enabled(void)
{ return option_tu_mode & LATX_TU_MODE_JIT; }
'''
body = "\n#ifdef CONFIG_LATX_TU\n" + function(
    "include/exec/translation-block.h", "static inline bool tb_is_tu(")
body += "\n#endif\n" + function(
    "target/i386/latx/optimization/sys-ras.c",
    "static inline uint32_t *latxs_ras_inst_ptr(")
body += "\n" + function("target/i386/latx/optimization/sys-ras.c",
                        "uint32_t latxs_ras_tp_closed_gate(")
body += "\n" + function("target/i386/latx/sbt/sys-aot-check.c",
                        "void aot_ras_clear(")
main = r'''
int main(void)
{
    uint32_t code[128];
    TranslationBlock tb = { .inst_ptr_offset = 24 * sizeof(uint32_t),
        .ras_tp_cold_offset = 64 * sizeof(uint32_t),
        .tc = { code + 32, 16 * sizeof(uint32_t) } };
    for (int jit_tu = 0; jit_tu <= 1; jit_tu++) {
        option_tu_mode = jit_tu ? LATX_TU_MODE_JIT : 0;
        for (int is_tu = 0; is_tu <= 1; is_tu++) {
            tb.bool_flags = is_tu ? IS_TU_TB : 0;
            int expected = 56;
#ifdef CONFIG_LATX_TU
            if (is_tu) expected -= 16;
#endif
            for (int i = 0; i < 128; i++) code[i] = 0xdeadbeef;
            assert(latxs_ras_inst_ptr(&tb) == code + expected);
            aot_ras_clear(&tb);
            for (int i = 0; i < 128; i++) {
                uint32_t value = 0xdeadbeef;
                if (i >= expected - 1 && i <= expected + 4) value = 0x03400000;
                if (i == expected - 2) value = 0x50000000 | (0x7 << 10);
                assert(code[i] == value);
            }
            option_rastoptp = 1;
            for (int i = 0; i < 128; i++) code[i] = 0xdeadbeef;
            aot_ras_clear(&tb);
            for (int i = 0; i < 128; i++) {
                uint32_t value = 0xdeadbeef;
                if (i == expected - 3) value = 0x0347fc40 | ((0xdeadbeefU >> 5)&31);
                if (i == expected) value = 0x50000000 | (40 << 10);
                if (i == expected + 1) value = 0x03400000;
                if (i == expected + 3) value &= ~(1U << 10);
                assert(code[i] == value);
            }
            tb.misc_flags=MF_RAS_CLEAR_B0;
            aot_ras_clear(&tb);
            assert(code[expected-3]==LATX_RAS_TP_CALL_SKIP);
            tb.misc_flags=0;
            aot_ras_clear(&tb);
            assert(code[expected-3]==(0x0347fc40|((0xdeadbeefU>>5)&31)));
            option_rastoptp = 0;
        }
    }
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="aot-tu-ras-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(prefix + body + main)
    for defines in ([], ["-DCONFIG_LATX_TU"]):
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                        "-Wextra", "-Werror"] +
                       shlex.split(os.environ.get("CFLAGS", "")) + defines +
                       [str(cfile), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30)
print("PASS: RAS offsets follow TB identity independently of JIT TU")
