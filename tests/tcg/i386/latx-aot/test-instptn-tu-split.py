#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check AOT IR1 optimization is deferred and stale diagnostics are debug-only."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
instptn_source = (root / "target/i386/latx/optimization/sys-inst-ptn.c").read_text()
tu_source = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
start = instptn_source.index("void __latxs_inst_pattern(")
end = instptn_source.index("\n}", start) + 2
pattern_function = instptn_source[start:end]

aot_start = tu_source.index("static TranslationBlock* aot_tb_create(")
aot_end = tu_source.index("\n}", aot_start) + 2
aot_tb_create_function = tu_source[aot_start:aot_end]
assert "latxs_inst_pattern(tb);" not in aot_tb_create_function
assert "latxs_flag_reduction(tb);" not in aot_tb_create_function

prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define INSTPTN_BUF_SIZE 4
#define INSTPTN_OPC_NONE 0
#define INSTPTN_OPC_CMP_SBB 0x100
#define INSTPTN_OPC_NOP 1

typedef int scan_elem_t;
typedef struct IR1_INST {
    struct {
        uint64_t opc;
        int next;
    } instptn;
} IR1_INST;
typedef struct TranslationBlock {
    int icount;
    uint64_t pc;
} TranslationBlock;
typedef struct TRANSLATION_DATA {
    IR1_INST *ir1_inst_array;
} TRANSLATION_DATA;
static TRANSLATION_DATA td;
static struct {
    TRANSLATION_DATA *tr_data;
} env = { &td }, *lsenv = &env;

static int option_instptn = 1;
static int filter_cpl;
static int scan_count;
static TranslationBlock tb;
static IR1_INST instructions[2];

#define latxs_instptn_check_void() do { \
    if (!option_instptn) return;        \
} while (0)
#define latxs_instptn_cpl_filter_void(tb) do { \
    if (filter_cpl) return;                    \
} while (0)

static int tb_ir1_num(TranslationBlock *block)
{
    return block->icount;
}

static IR1_INST *ir1_inst_in_list(IR1_INST *list, int index)
{
    return &list[index];
}

static void scan_clear(scan_elem_t *scan)
{
    memset(scan, -1, sizeof(scan_elem_t) * INSTPTN_BUF_SIZE);
}

static void inst_pattern_scan_con(TranslationBlock *block, scan_elem_t *scan,
                                  IR1_INST *ir1, int index)
{
    (void)scan;
    scan_count++;
    if (block->icount == 2 && index == 0) {
        ir1->instptn.opc = INSTPTN_OPC_CMP_SBB;
        ir1->instptn.next = 1;
        ir1_inst_in_list(lsenv->tr_data->ir1_inst_array, 1)->instptn.opc =
            INSTPTN_OPC_NOP;
    }
}

static bool inst_pattern_scan_jcc_end(TranslationBlock *block,
                                      scan_elem_t *scan, IR1_INST *ir1,
                                      int index)
{
    (void)block;
    (void)scan;
    (void)ir1;
    (void)index;
    return true;
}
"""

suffix = r"""
static void reset_test(int icount)
{
    memset(instructions, 0, sizeof(instructions));
    instructions[0].instptn.next = -1;
    instructions[1].instptn.next = -1;
    tb.icount = icount;
    tb.pc = 0x1234;
    env.tr_data->ir1_inst_array = instructions;
    option_instptn = 1;
    filter_cpl = 0;
    scan_count = 0;
}

static void set_stale_cmp_sbb(void)
{
    instructions[0].instptn.opc = INSTPTN_OPC_CMP_SBB;
    instructions[0].instptn.next = 1;
    instructions[1].instptn.opc = INSTPTN_OPC_NOP;
}

#ifdef CONFIG_LATX_DEBUG
static void expect_cmp_sbb_cleared(void)
{
    assert(instructions[0].instptn.opc == INSTPTN_OPC_NONE &&
           "stale CMP/SBB pattern survived debug cleanup");
    assert(instructions[0].instptn.next == -1 &&
           "stale CMP/SBB tail link survived debug cleanup");
}
#else
static void expect_cmp_sbb_stale(void)
{
    assert(instructions[0].instptn.opc == INSTPTN_OPC_CMP_SBB &&
           "release build unexpectedly cleared a stale pattern");
    assert(instructions[0].instptn.next == 1 &&
           "release build unexpectedly cleared a stale tail link");
}
#endif

int main(void)
{
    reset_test(2);
    __latxs_inst_pattern(&tb);
    assert(instructions[0].instptn.opc == INSTPTN_OPC_CMP_SBB);
    assert(instructions[0].instptn.next == 1);
    assert(instructions[1].instptn.opc == INSTPTN_OPC_NOP);
    assert(scan_count == 2);

    reset_test(1);
    set_stale_cmp_sbb();
    __latxs_inst_pattern(&tb);
#ifdef CONFIG_LATX_DEBUG
    expect_cmp_sbb_cleared();
#else
    expect_cmp_sbb_stale();
#endif
    assert(scan_count == 1);

    reset_test(2);
    set_stale_cmp_sbb();
    option_instptn = 0;
    __latxs_inst_pattern(&tb);
#ifdef CONFIG_LATX_DEBUG
    expect_cmp_sbb_cleared();
#else
    expect_cmp_sbb_stale();
#endif
    assert(scan_count == 0);

    reset_test(2);
    set_stale_cmp_sbb();
    filter_cpl = 1;
    __latxs_inst_pattern(&tb);
#ifdef CONFIG_LATX_DEBUG
    expect_cmp_sbb_cleared();
#else
    expect_cmp_sbb_stale();
#endif
    assert(scan_count == 0);
    return 0;
}
"""


def compile_and_run(function, tmp, name, enable_debug):
    source_path = tmp / f"{name}.c"
    executable = tmp / name
    source_path.write_text(prefix + function + suffix)
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        str(source_path), "-o", str(executable),
    ]
    if enable_debug:
        command.append("-DCONFIG_LATX_DEBUG")
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(f"{name} did not compile:\n{result.stderr}")
    result = subprocess.run([str(executable)], capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(f"{name} failed:\n{result.stderr}")
    if enable_debug:
        if "stale marker before rescan" not in result.stderr:
            raise AssertionError(f"{name} did not report stale metadata:\n"
                                 f"{result.stderr}")
    elif result.stderr:
        raise AssertionError(f"{name} emitted unexpected diagnostics:\n"
                             f"{result.stderr}")


with tempfile.TemporaryDirectory(prefix="instptn-tu-split-") as tmpdir:
    tmp = Path(tmpdir)
    compile_and_run(pattern_function, tmp, "debug", True)
    compile_and_run(pattern_function, tmp, "release", False)

print("PASS: AOT creation defers IR1 optimization; stale diagnostics are debug-only")
