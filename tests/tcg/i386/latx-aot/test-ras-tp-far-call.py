#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise the production cold far-call emitter beyond the BL range."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/optimization/sys-ras.c").read_text()
start = source.index("        int offset = (GET_SC_TABLE_OFFSET(td->mtcc_id, ras_tp_repair,")
end = source.index("\n    }\n    la_out1(LISA_B, &td->ras_tp_call_done);", start)
body = source[start:end]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef int IR2_OPND;
enum { LISA_BL, LISA_PCADDU18I, LISA_JIRL };
static const IR2_OPND la_ra = 1;
static int option_large_code_cache = 1, count;
static int64_t target, operands[2][3];
static int opcodes[2];
#define GET_SC_TABLE_OFFSET(...) target
#define get_current_inst_addr(...) 0
static bool int32_in_int26(int32_t x) { return x >= -(1 << 25) && x < (1 << 25); }
static void latxs_append_ir2_opnd1i(int op, const IR2_OPND *rd, int64_t imm) {
    assert(count < 2); opcodes[count]=op; operands[count][0]=*rd;
    operands[count++][2]=imm;
}
static void la_out2i(int op, const IR2_OPND *rd, const IR2_OPND *rj, int64_t imm) {
    assert(count < 2); opcodes[count]=op; operands[count][0]=*rd;
    operands[count][1]=*rj; operands[count++][2]=imm;
}
static void la_outa(int op, int64_t imm) {
    assert(count < 2); opcodes[count]=op; operands[count++][2]=imm;
}
static void emit(void) {
''' + body + r'''
}
int main(void) {
    int32_t offsets[] = {0, (1<<25)-1, -(1<<25), (1<<25), -(1<<25)-1,
                         (1<<29)+32767, (1<<29)+32768,
                         -(1<<29)-32768, -(1<<29)-32769};
    for (unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        target = (int64_t)offsets[i]*4; count=0; emit();
        if (int32_in_int26(offsets[i])) {
            assert(count==1 && opcodes[0]==LISA_BL);
            assert(operands[0][2]*4==target);
        } else {
            assert(count==2 && opcodes[0]==LISA_PCADDU18I && opcodes[1]==LISA_JIRL);
            assert(operands[0][0]==1 && operands[1][0]==1 && operands[1][1]==1);
            assert(operands[0][2]*262144+operands[1][2]*4==target);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix="ras-tp-far-") as directory:
    binary = Path(directory) / "test"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fwrapv",
                    "-x", "c", "-o", str(binary), "-"],
                   input=prefix, text=True, check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: far repair calls write only ra and reach the expected target")
