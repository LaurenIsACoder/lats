#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise real exception top handling and kernel/user prepare decisions."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/optimization/sys-ras.c").read_text()


def function(signature):
    begin = source.index(signature)
    return source[begin:source.index("\n}", begin) + 2]


prepare = function("void latxs_ras_tp_prepare(")
prepare = prepare.replace("#if defined(__loongarch__) && !defined(CONFIG_USER_ONLY)", "")
prepare = prepare.replace("#endif", "")
prepare = prepare.replace('asm volatile("or %0, $tp, $zero" : "=r"(native_tp));',
                          "native_tp = 0x123456789abc;")
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define LATX_RAS_TP_SIZE 512
#define HF_CPL_MASK 3
#define g_assert assert
#define qatomic_read(p) (*(p))
#define qatomic_set(p,v) (*(p)=(v))
#define qatomic_xchg(p,v) __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST)
typedef struct { uint64_t pc; const void *ptr; } LatxRasTPEntry;
typedef struct {
    uint32_t top, reset_pending, mtcc_id;
    uint64_t native_tp;
    bool initialized;
    LatxRasTPEntry sentinel, stack[512];
} LatxRasTPState;
typedef struct { LatxRasTPState ras_tp; uint32_t hflags; } CPUX86State;
typedef struct { CPUX86State *env_ptr; int cpu_index; } CPUState;
static int option_rastoptp = 1;
static bool qemu_tcg_mttcg_enabled(void) { return true; }
static void hamt_ras_tp_mirror(int cpuid, uint64_t tp) {
    assert(cpuid == 2 && tp == 0x123456789abc);
}
static bool hamt_mode_dmw(void) { return false; }
#define GET_SC_TABLE(id,name) (0x1000 + (id)*0x100)
#define DMW_IN(v) (v)
'''
checks = r'''
int main(void) {
    CPUX86State env = {0}; CPUState cpu = {&env, 2}; uint64_t frame[32] = {0};
    /* No array initialization: previously allocated entries remain untouched. */
    memset(env.ras_tp.stack, 0xa5, sizeof(env.ras_tp.stack));
    LatxRasTPEntry entries[512]; memcpy(entries, env.ras_tp.stack, sizeof(entries));
    latxs_ras_tp_prepare(&cpu, 0);
    assert(!env.ras_tp.initialized);
    env.hflags = 3;
    latxs_ras_tp_prepare(&cpu, 1);
    assert(env.ras_tp.initialized && env.ras_tp.mtcc_id == 1);
    assert(!env.ras_tp.top && !env.ras_tp.sentinel.pc);
    assert(env.ras_tp.sentinel.ptr == (void *)0x1100);
    assert(!memcmp(entries, env.ras_tp.stack, sizeof(entries)));
    env.ras_tp.top = 32; env.hflags = 0;
    latxs_ras_tp_prepare(&cpu, 0);
    assert(env.ras_tp.top == 32 && env.ras_tp.mtcc_id == 1);
    assert(env.ras_tp.sentinel.ptr == (void *)0x1100);
    env.hflags = 3; latxs_ras_tp_prepare(&cpu, 1);
    assert(env.ras_tp.top == 32);
    /* MTCC-off users own region 0; CPL, not region id, decides ownership. */
    latxs_ras_tp_prepare(&cpu, 0);
    assert(!env.ras_tp.top && env.ras_tp.mtcc_id == 0);
    env.ras_tp.top = 32; env.hflags = 0; env.ras_tp.reset_pending = 1;
    latxs_ras_tp_prepare(&cpu, 0);
    assert(!env.ras_tp.top && !env.ras_tp.reset_pending);
    /* Save for C/longjmp without altering the exact-PC frame. */
    frame[2] = 32; latxs_ras_tp_save(&env, frame);
    assert(env.ras_tp.top == 32 && frame[2] == 32);
    frame[2] = env.ras_tp.native_tp; latxs_ras_tp_save(&env, frame);
    assert(env.ras_tp.top == 32 && frame[2] == env.ras_tp.native_tp);
    latxs_ras_tp_redirect(&env, frame);
    assert(frame[2] == 32);
    frame[2] = 512; latxs_ras_tp_save(&env, frame);
    assert(env.ras_tp.top == 512);
    env.ras_tp.reset_pending = 1; frame[2] = 32;
    latxs_ras_tp_save(&env, frame);
    assert(!env.ras_tp.top && env.ras_tp.reset_pending && frame[2] == 32);
    latxs_ras_tp_redirect(&env, frame);
    assert(!frame[2] && !env.ras_tp.reset_pending);
    frame[2] = UINT64_MAX; latxs_ras_tp_save(&env, frame);
    assert(!env.ras_tp.top && env.ras_tp.reset_pending && frame[2] == UINT64_MAX);
    latxs_ras_tp_redirect(&env, frame);
    assert(!frame[2]);
    assert(!memcmp(entries, env.ras_tp.stack, sizeof(entries)));
}
'''
with tempfile.TemporaryDirectory(prefix="ras-tp-preserve-") as directory:
    binary = Path(directory) / "test"
    program = prefix + function("void latxs_ras_tp_save(") + function(
        "void latxs_ras_tp_redirect(") + prepare + checks
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-o", str(binary), "-"],
                   input=program, text=True, check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: kernel transitions preserve user RAS; real resets and unknown top clear it")
