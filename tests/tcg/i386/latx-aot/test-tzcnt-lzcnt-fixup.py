#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify BSF/BSR -> TZCNT/LZCNT IR1 fixup runs before flag reduction across prefix orders and bitnesses."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
sys_src = (root / "target/i386/latx/translator/sys.c").read_text()
setcc_src = (root / "target/i386/latx/translator/sys-tr-setcc.c").read_text()
disasm_src = (root / "target/i386/latx/translator/sys-disasm.c").read_text()


def extract_fn(src: str, name: str) -> str:
    pos = 0
    while True:
        idx = src.index(name + "(", pos)
        close = src.index(")", idx)
        rest = src[close + 1 :].lstrip()
        if rest.startswith("{"):
            start = src.rfind("\n", 0, idx) + 1
            brace = src.index("{", close)
            depth, end = 1, brace + 1
            while depth:
                depth += (src[end] == "{") - (src[end] == "}")
                end += 1
            return src[start:end]
        pos = close + 1


# 1. Verify latxs_fix_up_ir1 runs during disassembly (before flag reduction) and
#    latxs_translate_bsr/bsf cleanly dispatch X86_INS_LZCNT/X86_INS_TZCNT.
ir1_disasm_fn = extract_fn(disasm_src, "latxs_ir1_disasm")
assert "latxs_fix_up_ir1(ir1);" in ir1_disasm_fn
bsr_fn = extract_fn(setcc_src, "latxs_translate_bsr")
bsf_fn = extract_fn(setcc_src, "latxs_translate_bsf")
assert "ir1_opcode(pir1) == X86_INS_LZCNT" in bsr_fn
assert "ir1_opcode(pir1) == X86_INS_TZCNT" in bsf_fn

# 2. Extract and compile latxs_fix_up_ir1 directly from sys.c.
fixup_fn = extract_fn(sys_src, "latxs_fix_up_ir1")

program = (
    r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t uint8;
typedef int64_t int64;

enum {
    X86_REG_SS = 1,
    X86_REG_DS,
    X86_REG_MM0 = 10,
    X86_REG_XMM0 = 20,
    esp_index = 4,
    ebp_index = 5,
    X86_INS_INVALID = 0,
    X86_INS_BSF = 100,
    X86_INS_BSR,
    X86_INS_TZCNT,
    X86_INS_LZCNT,
    X86_INS_INSB,
    X86_INS_MOVSB,
    X86_INS_CMPSB,
    X86_INS_LODSB,
    X86_INS_STOSB,
    X86_INS_OUTSB,
    X86_INS_SCASB,
    X86_INS_MOVD,
    X86_INS_MOVQ,
};

typedef struct {
    int type;
    int size;
    int reg;
    struct {
        int64 disp;
        int segment;
        int base;
    } mem;
} IR1_OPND;

typedef struct {
    uint8 modrm;
    int64 disp;
    uint8 op_count;
    IR1_OPND operands[4];
} cs_x86_stub;

typedef struct {
    cs_x86_stub x86;
} cs_detail_stub;

typedef struct {
    unsigned id;
    uint16_t size;
    uint8_t bytes[16];
    cs_detail_stub *detail;
} cs_insn_stub;

typedef struct {
    cs_insn_stub *info;
} IR1_INST;

typedef struct {
    struct {
        bool addseg;
        bool code64;
    } sys;
} TRANSLATION_DATA;

typedef struct {
    TRANSLATION_DATA *tr_data;
} ENV;

static TRANSLATION_DATA td_state;
static ENV env_state = { .tr_data = &td_state };
static ENV *lsenv = &env_state;

#define lsassert(c) assert(c)
static int latxs_ir1_addr_size(IR1_INST *ir1) { (void)ir1; return 8; }
static bool latxs_ir1_is_lea(IR1_INST *ir1) { (void)ir1; return false; }
static IR1_OPND *ir1_get_opnd(IR1_INST *ir1, int i) { return &ir1->info->detail->x86.operands[i]; }
static bool ir1_opnd_is_mem(IR1_OPND *opnd) { return opnd->type == 2; }
static bool ir1_opnd_is_gpr(IR1_OPND *opnd) { return opnd->type == 1; }
static int ir1_opnd_size(IR1_OPND *opnd) { return opnd->size; }
static bool ir1_opnd_has_seg(IR1_OPND *opnd) { return opnd->mem.segment != 0; }
static int ir1_opnd_base_reg_num(IR1_OPND *opnd) { return opnd->mem.base; }
static bool latxs_ir1_is_string_op(IR1_INST *ir1) { (void)ir1; return false; }
static uint8_t *latxs_ir1_inst_opbytes(IR1_INST *ir1) { return ir1->info->bytes; }
static unsigned ir1_opcode(IR1_INST *ir1) { return ir1->info->id; }
static int latxs_ir1_data_size(IR1_INST *ir1) { (void)ir1; return 32; }
static void ir1_dump(IR1_INST *ir1) { (void)ir1; }
"""
    + "\n"
    + fixup_fn
    + r"""
static unsigned run_fixup(unsigned init_id, const uint8_t *bytes, size_t n, bool code64)
{
    cs_detail_stub detail = { 0 };
    cs_insn_stub info = { .id = init_id, .size = (uint16_t)n, .detail = &detail };
    IR1_INST ir1 = { .info = &info };
    memcpy(info.bytes, bytes, n);
    td_state.sys.code64 = code64;
    latxs_fix_up_ir1(&ir1);
    return info.id;
}

int main(void)
{
    /* Standard F3 0F BD / F3 0F BC in both 64-bit and 32-bit modes. */
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0xf3, 0x0f, 0xbd, 0xc0 }, 4, true) == X86_INS_LZCNT);
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0xf3, 0x0f, 0xbd, 0xc0 }, 4, false) == X86_INS_LZCNT);
    assert(run_fixup(X86_INS_BSF, (uint8_t[]){ 0xf3, 0x0f, 0xbc, 0xc0 }, 4, true) == X86_INS_TZCNT);
    assert(run_fixup(X86_INS_BSF, (uint8_t[]){ 0xf3, 0x0f, 0xbc, 0xc0 }, 4, false) == X86_INS_TZCNT);

    /* Both 66 F3 and F3 66 operand-size / REP prefix orders in 64-bit and 32-bit modes. */
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0x66, 0xf3, 0x0f, 0xbd, 0xc0 }, 5, true) == X86_INS_LZCNT);
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0x66, 0xf3, 0x0f, 0xbd, 0xc0 }, 5, false) == X86_INS_LZCNT);
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0xf3, 0x66, 0x0f, 0xbd, 0xc0 }, 5, true) == X86_INS_LZCNT);
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0xf3, 0x66, 0x0f, 0xbd, 0xc0 }, 5, false) == X86_INS_LZCNT);
    assert(run_fixup(X86_INS_BSF, (uint8_t[]){ 0x66, 0xf3, 0x0f, 0xbc, 0xc0 }, 5, true) == X86_INS_TZCNT);
    assert(run_fixup(X86_INS_BSF, (uint8_t[]){ 0x66, 0xf3, 0x0f, 0xbc, 0xc0 }, 5, false) == X86_INS_TZCNT);
    assert(run_fixup(X86_INS_BSF, (uint8_t[]){ 0xf3, 0x66, 0x0f, 0xbc, 0xc0 }, 5, true) == X86_INS_TZCNT);
    assert(run_fixup(X86_INS_BSF, (uint8_t[]){ 0xf3, 0x66, 0x0f, 0xbc, 0xc0 }, 5, false) == X86_INS_TZCNT);

    /* Address-size (0x67) and segment prefixes before F3. */
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0x67, 0xf3, 0x0f, 0xbd, 0x00 }, 5, true) == X86_INS_LZCNT);
    assert(run_fixup(X86_INS_BSF, (uint8_t[]){ 0x2e, 0x67, 0xf3, 0x0f, 0xbc, 0x00 }, 6, false) == X86_INS_TZCNT);

    /* REX (0x40..0x4f) is valid in 64-bit mode, but NOT a prefix in 32-bit mode. */
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0x48, 0xf3, 0x0f, 0xbd, 0xc0 }, 5, true) == X86_INS_LZCNT);
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0xf3, 0x48, 0x0f, 0xbd, 0xc0 }, 5, true) == X86_INS_LZCNT);
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0x48, 0xf3, 0x0f, 0xbd, 0xc0 }, 5, false) == X86_INS_BSR);

    /* Last repeat prefix wins: F2 F3 -> LZCNT/TZCNT, F3 F2 -> BSR/BSF, no F3 -> BSR/BSF. */
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0xf2, 0xf3, 0x0f, 0xbd, 0xc0 }, 5, true) == X86_INS_LZCNT);
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0xf3, 0xf2, 0x0f, 0xbd, 0xc0 }, 5, true) == X86_INS_BSR);
    assert(run_fixup(X86_INS_BSF, (uint8_t[]){ 0xf3, 0xf2, 0x0f, 0xbc, 0xc0 }, 5, true) == X86_INS_BSF);
    assert(run_fixup(X86_INS_BSR, (uint8_t[]){ 0x66, 0x0f, 0xbd, 0xc0 }, 4, true) == X86_INS_BSR);
    assert(run_fixup(X86_INS_BSF, (uint8_t[]){ 0x66, 0x0f, 0xbc, 0xc0 }, 4, true) == X86_INS_BSF);

    puts("PASS: BSF/BSR -> TZCNT/LZCNT IR1 fixup handles prefix orders and bitnesses");
    return 0;
}
"""
)

with tempfile.TemporaryDirectory(prefix="lats-tzcnt-lzcnt-") as folder:
    binary = Path(folder) / "check"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
         "-x", "c", "-o", str(binary), "-"],
        input=program,
        text=True,
        check=True,
    )
    subprocess.run([str(binary)], check=True)
