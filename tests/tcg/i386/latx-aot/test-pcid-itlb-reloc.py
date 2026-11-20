#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise the production fast-iTLB relocation with both cache layouts."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
reloc = (root / "target/i386/latx/sbt/sys-aot-reloc.c").read_text()
eob = (root / "target/i386/latx/translator/sys-eob.c").read_text()
case = reloc.split("case LOAD_BY_GUEST_ADDR_CPU_NEG_ITLB:", 1)[1]
case = case.split("            break;", 1)[0]
branch = eob.split("static inline void tb_gen_cpu_neg_itlb", 1)[1]
branch = branch.split("    int off = 0;", 1)[0]
assert branch.index("LISA_LD_D") < branch.index("add_curr_rel_entry")
assert branch.index("add_curr_rel_entry") < branch.index("LISA_LDPTR_D")

source = r"""
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define HF_CPL_MASK 3
#define FAST_ITLB_INDEX(a) (((a) >> 12) & 1023)
typedef uint64_t fast_itlb_entry_t;
typedef struct { struct { char itlb_user[8192], itlb_knosmap[8192]; } neg;
                 char env; } ArchCPU;
typedef struct { uint64_t pc; int flags; } TB;
typedef struct { int64_t extra_addend; } Rel;
static int option_pcid_cache;
static uint32_t relocate(TB *tb, Rel *aot_rel_table) {
    int i = 0, offset;
    uint64_t guest_addr;
    uint32_t insn = 0, *pinsn = &insn;
""" + case + r"""
    return insn;
}
int main(void) {
    for (int bank = 0; bank < 2; bank++) {
        for (int user = 0; user < 2; user++) {
            option_pcid_cache = bank;
            for (unsigned n = 0; n < 1024; n++) {
                TB tb = { .pc = ((uint64_t)n << 12), .flags = user ? 3 : 0 };
                Rel rel = { .extra_addend = 0x3000 };
                int base = bank ? 0 : user ?
                    offsetof(ArchCPU, neg.itlb_user) - offsetof(ArchCPU, env) :
                    offsetof(ArchCPU, neg.itlb_knosmap) - offsetof(ArchCPU, env);
                unsigned imm = ((base + FAST_ITLB_INDEX(tb.pc + 0x3000) * 8) >> 2) & 0x3fff;
                uint32_t expected = 0x26000000 | 4 | ((bank ? 4 : 23) << 5) | (imm << 10);
                assert(relocate(&tb, &rel) == expected);
            }
        }
    }
}
"""
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "test.c").write_text(source)
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: fast iTLB relocation, 4096 bank/CPL/address cases")
