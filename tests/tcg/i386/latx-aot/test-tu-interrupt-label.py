#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check TU interrupt labels with JIT TU disabled and compact sidecar IDs."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/ir2/ir2.c").read_text()


def function(signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
enum { IR2_OPND_LABEL = 123 };
typedef struct { int type; int value; } IR2_OPND;
typedef struct { int is_tu_mode; IR2_OPND label_intexit[2]; } TRANSLATION_DATA;
struct sidecar { uint16_t label_intexit[2]; uint32_t guard; };
typedef struct { struct sidecar *s_data; } TranslationBlock;
static TRANSLATION_DATA data;
static struct { TRANSLATION_DATA *tr_data; } env = { &data }, *lsenv = &env;
static unsigned option_tu_mode;
#define LATX_TU_MODE_JIT (1u << 0)
static inline bool jit_tu_enabled(void)
{ return option_tu_mode & LATX_TU_MODE_JIT; }
static IR2_OPND *emitted;
#ifdef CONFIG_LATX_TU
static IR2_OPND latxs_ir2_opnd_new(int type, int value)
{ return (IR2_OPND){ type, value }; }
#endif
static void la_label_ptr(IR2_OPND *label) { emitted = label; }
'''

body = function("IR2_OPND *latxs_ir2_interrupt_get_label(") + "\n"
body += function("void latxs_ir2_interrupt_set_label(")
main = r'''
int main(void)
{
    struct sidecar saved = { { 17, 29 }, 0x12345678 };
    struct sidecar original = saved;
    TranslationBlock tb = { &saved };
    for (int jit_tu = 0; jit_tu <= 1; jit_tu++) {
        option_tu_mode = jit_tu ? LATX_TU_MODE_JIT : 0;
        for (int tu_mode = 0; tu_mode <= 1; tu_mode++) {
            data.is_tu_mode = tu_mode;
            for (int edge = 0; edge < 2; edge++) {
                for (int setter = 0; setter <= 1; setter++) {
                    data.label_intexit[edge] = (IR2_OPND){ IR2_OPND_LABEL, 71 };
                    emitted = NULL;
                    IR2_OPND *label;
                    if (setter) {
                        latxs_ir2_interrupt_set_label(&tb, edge);
                        label = emitted;
                    } else {
                        label = latxs_ir2_interrupt_get_label(&tb, edge);
                    }
                    int expected = 71;
#ifdef CONFIG_LATX_TU
                    if (tu_mode) expected = saved.label_intexit[edge];
#endif
                    assert(label == &data.label_intexit[edge]);
                    assert(label->type == IR2_OPND_LABEL);
                    assert(label->value == expected);
                    assert(!memcmp(&saved, &original, sizeof(saved)));
                }
            }
        }
    }
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="aot-tu-label-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(prefix + body + main)
    for defines in ([], ["-DCONFIG_LATX_TU"]):
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                        "-Wextra", "-Werror", "-Wno-unused-parameter"] +
                       shlex.split(os.environ.get("CFLAGS", "")) + defines +
                       [str(cfile), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30)
print("PASS: interrupt labels preserve compact IDs independently of JIT TU")
