#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run the production pre-disassembler policy against bounded synthetic bytes."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
start = source.index("static int tb_disasm_eflag_use_zero(")
end = source.index("static void tu_tblist_optimize(", start)
harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>
#define TARGET_PAGE_MASK (~(uint64_t)4095)
#define HF_CS64_MASK 1
#define HF_CS32_MASK 2
#define LATXS_IR1_FLAGS_GENNOP 1
#define __ALL_EFLAGS 63
#define IR1_INST_SIZE 64
typedef uint64_t target_ulong;
typedef uint64_t tb_page_addr_t;
typedef int CPUX86State;
typedef struct { void *env_ptr; } CPUState;
typedef struct { uintptr_t hva; target_ulong pc; } TranslationBlock;
typedef struct { unsigned use, def, undef; } IR1_EFLAG_USEDEF;
typedef struct { int size; } Info;
typedef struct { Info *info; int flags; unsigned char opcode; } IR1_INST;
typedef struct {
    struct { target_ulong pc, cs_base; uint32_t flags, cflags; } sys;
} TRANSLATION_DATA;
static TRANSLATION_DATA td;
static struct { TRANSLATION_DATA *tr_data; } env = { &td }, *lsenv = &env;
static void *tb_ir1_info;
static bool in_aot_translate = true, enabled = true;
static int option_aot_tu_pre_disasm;
static int lookups, decodes;
static unsigned char *snapshot;
static uint64_t get_page(uint64_t pc) { return pc & TARGET_PAGE_MASK; }
static bool inst_may_cross_page(uint64_t pc)
{ return get_page(pc) != get_page(pc + 14); }
static bool flag_reduction_tu_pre_disasm(void) { return enabled; }
static uint64_t get_page_addr_code_hostp_noexit(void *env, uint64_t pc, void **host)
{
    assert(!in_aot_translate);
    lookups++;
    *host = snapshot + (pc & 4095);
    return pc;
}
static int tb_disasm_one(IR1_INST *ir, void *addr, uint64_t pc, int mode,
                         void *base, int i)
{
    static Info info;
    assert(mode == 64 || mode == 32 || mode == 16);
    assert(addr >= (void *)snapshot && addr <= (void *)(snapshot + 4096 - 15));
    ir->opcode = *(unsigned char *)addr;
    info.size = ir->opcode == 15 ? 15 : 1;
    ir->info = &info;
    ir->flags = 0;
    decodes++;
    return ir->opcode == 255;
}
static bool latxs_ir1_is_illegal(IR1_INST *ir) { return ir->opcode == 254; }
static bool ir1_is_tb_ending(IR1_INST *ir) { return ir->opcode == 253; }
static bool latxs_ir1_is_eob_in_sys(IR1_INST *ir) { return false; }
static bool ir1_is_call(IR1_INST *ir) { return false; }
static bool ir1_is_indirect_call(IR1_INST *ir) { return false; }
static IR1_EFLAG_USEDEF latxs_ir1_get_tu_eflag_usedef(IR1_INST *ir)
{
    return (IR1_EFLAG_USEDEF) { .use = ir->opcode == 2,
                               .def = ir->opcode == 1 ? __ALL_EFLAGS : 0 };
}
"""
harness += source[start:end]
harness += r"""
int main(void)
{
    long host_page = sysconf(_SC_PAGESIZE);
    assert(host_page >= 4096);
    unsigned char *mapping = mmap(NULL, 2 * host_page, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(mapping != MAP_FAILED);
    snapshot = mapping + host_page - 4096;
    assert(!mprotect(mapping + host_page, host_page, PROT_NONE));
    CPUState cpu = {0};
    TranslationBlock tb = { (uintptr_t)snapshot, 0x1100 };
    td.sys.pc = 0x999; td.sys.cs_base = 7; td.sys.flags = 123; td.sys.cflags = 456;
    TRANSLATION_DATA saved = td;
    snapshot[0x200] = 1; /* defines all incoming flags without reading them */
    assert(!tb_disasm_eflag_use_zero(&tb, &cpu, 0x1200, 0, 1, 0));
    assert(decodes == 0 && lookups == 0);
    option_aot_tu_pre_disasm = 1;
    assert(tb_disasm_eflag_use_zero(&tb, &cpu, 0x1200, 0, 1, 0) == 1);
    assert(!memcmp(&saved, &td, sizeof(td)) && lookups == 0);
    snapshot[0x200] = 2; /* consumes an incoming flag */
    snapshot[0x201] = 1;
    assert(!tb_disasm_eflag_use_zero(&tb, &cpu, 0x1200, 0, 2, 0));
    for (int op = 253; op <= 255; op++) {
        snapshot[0x200] = op;
        assert(!tb_disasm_eflag_use_zero(&tb, &cpu, 0x1200, 0, 0, 0));
    }
    int old = decodes;
    assert(!tb_disasm_eflag_use_zero(&tb, &cpu, 0x2000, 0, 1, 0));
    assert(!tb_disasm_eflag_use_zero(&tb, &cpu, 0x1fff, 0, 1, 0));
    assert(decodes == old && lookups == 0);
    snapshot[0xff1] = 15; /* exactly ends at the snapshot boundary */
    assert(!tb_disasm_eflag_use_zero(&tb, &cpu, 0x1ff1, 0, 1, 0));
    assert(decodes == old + 1);
    memset(snapshot, 0, 4096);
    old = decodes;
    assert(!tb_disasm_eflag_use_zero(&tb, &cpu, 0x1200, 0, 1, 0));
    assert(decodes == old + 64);
    enabled = false;
    assert(!tb_disasm_eflag_use_zero(&tb, &cpu, 0x1200, 0, 1, 0));
    assert(decodes == old + 64);
    enabled = true;
    option_aot_tu_pre_disasm = 0;
    in_aot_translate = false;
    snapshot[0x200] = 1;
    assert(tb_disasm_eflag_use_zero(&tb, &cpu, 0x1200, 0, 1, 0));
    assert(lookups == 1 && !memcmp(&saved, &td, sizeof(td)));
    munmap(mapping, 2 * host_page);
    munmap(tb_ir1_info, 64 * IR1_INST_SIZE);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="aot-tu-pre-disasm-") as tmp:
    src = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    src.write_text(harness)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu11", "-Wall", "-Werror", str(src), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: AOT pre-disassembly switch, bounds, state restoration and "
      "JIT path")
