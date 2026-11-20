#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise the production operand-aware EFLAGS summary and reduction."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile


root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/optimization/sys-flag-rdtn.c").read_text()
start = source.index("static inline uint32_t rotate_shift_get_masked_imm")
end = source.index("static void __latxs_flag_reduction_parse_tb_use", start)

harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t uint8;
typedef unsigned IR1_OPCODE;
typedef struct {
    int type;
    int size;
    unsigned long long imm;
} IR1_OPND;
typedef struct {
    IR1_OPCODE opcode;
    int prefix;
    int eflag_opt;
    IR1_OPND opnds[3];
    uint8_t eflag_use;
    uint8_t eflag_def;
} IR1_INST;
typedef struct { uint8_t use, def, undef; } IR1_EFLAG_USEDEF;
typedef struct { struct { int code64; } sys; } TRANSLATION_DATA;
static TRANSLATION_DATA td;
static struct { TRANSLATION_DATA *tr_data; } env = { &td }, *lsenv = &env;

enum {
    X86_INS_ADD,
    X86_INS_RCL,
    X86_INS_RCR,
    X86_INS_ROL,
    X86_INS_ROR,
    X86_INS_SAL,
    X86_INS_SAR,
    X86_INS_SHL,
    X86_INS_SHR,
    X86_INS_SHLD,
    X86_INS_SHRD,
    X86_INS_CMPSB,
    X86_INS_CMPSW,
    X86_INS_CMPSD,
    X86_INS_CMPSQ,
    X86_INS_SCASB,
    X86_INS_SCASW,
    X86_INS_SCASD,
    X86_INS_SCASQ,
};
enum { X86_OP_REG, X86_OP_IMM };
enum { X86_PREFIX_NONE, X86_PREFIX_REPE, X86_PREFIX_REPNE };
enum { IR1_EFOPT_INIT, IR1_EFOPT_NONE };

#define __CF         (1u << 0)
#define __PF         (1u << 1)
#define __AF         (1u << 2)
#define __ZF         (1u << 3)
#define __SF         (1u << 4)
#define __OF         (1u << 5)
#define __NONE       0
#define __OSZPF      (__OF | __SF | __ZF | __PF)
#define __ALL_EFLAGS (__OF | __SF | __ZF | __AF | __PF | __CF)

static IR1_OPCODE ir1_opcode(IR1_INST *ir) { return ir->opcode; }
static IR1_OPND *ir1_get_opnd(IR1_INST *ir, int i) { return &ir->opnds[i]; }
static bool ir1_opnd_is_imm(IR1_OPND *opnd) { return opnd->type == X86_OP_IMM; }
static int ir1_opnd_size(IR1_OPND *opnd) { return opnd->size; }
static unsigned long long ir1_opnd_uimm(IR1_OPND *opnd) { return opnd->imm; }
static int ir1_prefix(IR1_INST *ir) { return ir->prefix; }
static void ir1_set_eflag_use(IR1_INST *ir, uint8_t use)
{
    ir->eflag_use = use;
}
static void ir1_set_eflag_def(IR1_INST *ir, uint8_t def)
{
    ir->eflag_def = def;
}

static const IR1_EFLAG_USEDEF *
latxs_ir1_opcode_to_eflag_usedef(IR1_INST *ir)
{
    static const IR1_EFLAG_USEDEF none = {0, 0, 0};
    static const IR1_EFLAG_USEDEF all = {0, __ALL_EFLAGS, 0};
    static const IR1_EFLAG_USEDEF shift = {0, __ALL_EFLAGS, __AF};
    static const IR1_EFLAG_USEDEF rotate = {0, __OF | __CF, 0};

    if (ir->eflag_opt == IR1_EFOPT_NONE) {
        return &none;
    }
    if (ir->opcode >= X86_INS_RCL && ir->opcode <= X86_INS_ROR) {
        return &rotate;
    }
    if ((ir->opcode >= X86_INS_SAL && ir->opcode <= X86_INS_SHR) ||
        ir->opcode == X86_INS_SHLD || ir->opcode == X86_INS_SHRD) {
        return &shift;
    }
    return &all;
}
"""
harness += source[start:end]
harness += r"""
static IR1_INST make_inst(IR1_OPCODE opcode, int bits,
                          int count_type, unsigned count)
{
    IR1_INST ir = { .opcode = opcode };
    ir.opnds[0] = (IR1_OPND) { X86_OP_REG, bits, 0 };
    ir.opnds[1] = (IR1_OPND) { count_type, 8, count };
    ir.opnds[2] = (IR1_OPND) { count_type, 8, count };
    return ir;
}

static void expect_summary(IR1_INST *ir, uint8_t use, uint8_t def)
{
    IR1_EFLAG_USEDEF ud = latxs_ir1_get_tu_eflag_usedef(ir);
    assert(ud.use == use);
    assert(ud.def == def);
}

int main(void)
{
    IR1_INST ir = make_inst(X86_INS_ADD, 32, X86_OP_REG, 0);
    expect_summary(&ir, 0, __ALL_EFLAGS);

    ir.eflag_opt = IR1_EFOPT_NONE;
    expect_summary(&ir, 0, 0);

    for (IR1_OPCODE op = X86_INS_SAL; op <= X86_INS_SHR; op++) {
        ir = make_inst(op, 32, X86_OP_REG, 0);
        expect_summary(&ir, 0, 0);
        ir = make_inst(op, 32, X86_OP_IMM, 0);
        expect_summary(&ir, 0, 0);
        ir = make_inst(op, 32, X86_OP_IMM, 32);
        expect_summary(&ir, 0, 0);
        ir = make_inst(op, 32, X86_OP_IMM, 1);
        expect_summary(&ir, 0, __ALL_EFLAGS);
    }

    td.sys.code64 = 1;
    ir = make_inst(X86_INS_SHL, 64, X86_OP_IMM, 64);
    expect_summary(&ir, 0, 0);
    ir = make_inst(X86_INS_SHL, 64, X86_OP_IMM, 32);
    expect_summary(&ir, 0, __ALL_EFLAGS);

    for (IR1_OPCODE op = X86_INS_RCL; op <= X86_INS_ROR; op++) {
        ir = make_inst(op, 64, X86_OP_REG, 0);
        expect_summary(&ir, 0, 0);
        ir = make_inst(op, 64, X86_OP_IMM, 0);
        expect_summary(&ir, 0, 0);
        ir = make_inst(op, 64, X86_OP_IMM, 1);
        expect_summary(&ir, 0, __OF | __CF);
    }

    for (IR1_OPCODE op = X86_INS_SHLD; op <= X86_INS_SHRD; op++) {
        ir = make_inst(op, 64, X86_OP_REG, 0);
        ir.opnds[1].type = X86_OP_REG;
        expect_summary(&ir, 0, 0);
        ir = make_inst(op, 64, X86_OP_IMM, 1);
        ir.opnds[1].type = X86_OP_REG;
        expect_summary(&ir, 0, __ALL_EFLAGS);
        ir.opnds[2].imm = 64;
        expect_summary(&ir, 0, 0);
    }

    ir = make_inst(X86_INS_CMPSB, 8, X86_OP_REG, 0);
    ir.prefix = X86_PREFIX_REPE;
    expect_summary(&ir, 0, __ALL_EFLAGS);
    uint8_t pending = __ALL_EFLAGS;
    __latxs_flag_reduction_ir1(&ir, &pending);
    assert(ir.eflag_use == __ZF);

    ir = make_inst(X86_INS_SHL, 32, X86_OP_REG, 0);
    pending = __ALL_EFLAGS;
    __latxs_flag_reduction_ir1(&ir, &pending);
    assert(ir.eflag_def == (__ALL_EFLAGS & ~__AF));
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="tu-eflag-summary-") as tmp:
    src = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    src.write_text(harness)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu11", "-Wall", "-Werror", str(src), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)

print("PASS: TU EFLAGS summaries handle operand-dependent counts")
