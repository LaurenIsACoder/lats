#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the emitted indirect CALL resolver against its caller's stack frame."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
loader = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
header = (root / "target/i386/latx/include/translate.h").read_text()
frame = re.search(r"#ifdef CONFIG_LATX_OPT_NOFP\n#define LATXS_SCS_MMX_FRAME_SIZE.*?#endif", header, re.S)[0]


def function(name):
    start = loader.rfind("\n", 0, loader.index(name + "(")) + 1
    brace = loader.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (loader[end] == "{") - (loader[end] == "}")
        end += 1
    return loader[start:end]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define QEMU_BUILD_BUG_ON(c) _Static_assert(!(c), "build assertion")
#define CF_INVALID (1U << 18)
#define LATX_RAS_TP_SIZE 512
#define AOT_INDIRECT_COLD_BYTES 256
#define GET_SC_TABLE(cpu, name) ((void)(cpu), table.name)
static uint32_t code[512];
static struct { uintptr_t scs_prologue, scs_epilogue, cs_native_to_bt_ret_0; } table;
typedef struct { uint64_t stack[LATX_RAS_TP_SIZE * 2]; unsigned reset_pending; } LatxRasTPState;
typedef struct {
    uint64_t eip, tu_generation_array;
    LatxRasTPState ras_tp;
} CPUArchState;
typedef struct { unsigned cflags; } TranslationBlock;
static bool scs_enabled(void) { return true; }
static int64_t sextract64(uint64_t value, unsigned start, unsigned length)
{ return (int64_t)(value << (64 - start - length)) >> (64 - length); }
static void flush_idcache_range(uintptr_t first, uintptr_t last, unsigned bytes)
{ (void)first; (void)last; (void)bytes; }
static void latx_helper_aot_indirect_call(void) {}
'''
checks = r'''
/* Execute the stack-addressing instructions around the real C-call site.
 * The caller's frame must have the same contents after the shared helper's
 * private MMX frame is removed, including its original-SP slot. */
static int displacement(uint32_t instruction)
{ return (int)sextract64(instruction, 10, 12); }
int main(void)
{
    uint32_t *slot = code + 80, *poison = code + 120, *stub = code + 160;
    table.scs_prologue = (uintptr_t)(code + 300);
    table.scs_epilogue = (uintptr_t)(code + 320);
    table.cs_native_to_bt_ret_0 = (uintptr_t)(code + 340);
    *slot = 0x4c0020c1;
    {
        uint32_t *branch = slot - 1;
        uint64_t offset = poison - branch;
        *branch = 0x40000120 | ((offset & 65535) << 10) | ((offset >> 16) & 31);
    }
    assert(aot_native_indirect_resolver(stub, slot, NULL, poison, 0));
    int64_t miss_off = sextract64(((slot[-1] & 31U) << 16) | ((slot[-1] >> 10) & 65535U), 0, 21);
    assert((slot - 1) + miss_off == stub);
    assert((stub + 0) + sextract64(((stub[0] & 31U) << 16) | ((stub[0] >> 10) & 65535U), 0, 21) == stub + 5);
    assert((stub + 2) + sextract64(stub[2] >> 10, 0, 16) == stub + 5);
    assert((stub + 4) + sextract64(stub[4] >> 10, 0, 16) == stub + 55);
    assert(stub[51] == 0x50001000U);
    assert(stub[61] == 0x02ff80c6U);
    int64_t cont_off = sextract64((stub[58] >> 5) & 0xfffffU, 0, 20);
    assert((stub + 58) + cont_off == slot + 1);
    int64_t back_off = sextract64(((stub[62] & 1023U) << 16) | ((stub[62] >> 10) & 65535U), 0, 26);
    assert((stub + 62) + back_off == slot);
    uint64_t stack[16];
    for (unsigned index = 0; index < 16; ++index) stack[index] = 0xdeadbeef;
    uint64_t *caller = stack + 4;
    caller[1] = UINT64_C(0x1234567800); /* guest target */
    caller[3] = UINT64_C(0x9000000000100000); /* original SP form */
    uintptr_t sp = (uintptr_t)caller - LATXS_SCS_MMX_FRAME_SIZE;
    assert((stub[23] & 0xffc003ff) == 0x28c00066); /* ld.d a2, sp */
    assert(*(uint64_t *)(sp + displacement(stub[23])) == caller[1]);
    assert((stub[24] & 0xffc003ff) == 0x02c00067); /* addi.d a3, sp */
    uint64_t *output = (uint64_t *)(sp + displacement(stub[24]));
    assert(output == caller + 5);
    output[0] = 17;
    output[1] = 0x7788;
    assert((stub[30] & 0xffc003ff) == 0x29c00064); /* st.d a0, sp */
    *(uint64_t *)(sp + displacement(stub[30])) = 0x4455;
    sp += LATXS_SCS_MMX_FRAME_SIZE;
    assert(*(uint64_t *)(sp + displacement(stub[34])) == 0x4455);
    assert(*(uint64_t *)(sp + displacement(stub[40])) == 0x7788);
    assert(*(uint64_t *)(sp + displacement(stub[44])) == 17);
    assert(caller[3] == UINT64_C(0x9000000000100000));
    assert(stack[0] == 0xdeadbeef && caller[7] == 0xdeadbeef);
}
'''
program = (prefix + frame + "\n" + function("aot_call_load64") + "\n" + function("aot_call_far") +
           "\n" + function("aot_native_indirect_resolver") + checks)
with tempfile.TemporaryDirectory(prefix="aot-indirect-frame-") as folder:
    binary = Path(folder) / "check"
    for nofp in (False, True):
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        *(["-DCONFIG_LATX_OPT_NOFP"] if nofp else []),
                        "-x", "c", "-o", str(binary), "-"], input=program, text=True, check=True)
        subprocess.run([str(binary)], check=True)
print("PASS: indirect CALL target, outputs and original SP survive shared MMX frame")
