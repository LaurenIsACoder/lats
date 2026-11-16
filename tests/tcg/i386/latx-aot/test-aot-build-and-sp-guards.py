#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify AOT build guards (NOFP, SOFTMMU) and paired outer-SP save/restore."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]


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


# 1. Verify meson.build and shared C/H files gate system-only AOT code under CONFIG_SOFTMMU.
sbt_meson = (root / "target/i386/latx/sbt/meson.build").read_text()
m_uncond = re.search(r"i386_ss\.add\(files\((.*?)\)\)", sbt_meson, re.S)
m_softmmu = re.search(
    r"i386_ss\.add\(when:\s*'CONFIG_SOFTMMU',\s*if_true:\s*files\((.*?)\)\)",
    sbt_meson,
    re.S,
)
assert m_uncond and m_softmmu, "missing unconditional or CONFIG_SOFTMMU i386_ss block"
for f in (
    "sys-aot-offline.c",
    "sys-aot-runtime-info.c",
    "sys-aot-atomic.c",
    "sys-aot-cpu-query.c",
    "sys-aot-system.c",
    "sys-aot-x87.c",
):
    assert f not in m_uncond.group(1), f"{f} must not be unconditionally in i386_ss"
    assert f in m_softmmu.group(1), f"{f} must be gated by CONFIG_SOFTMMU"

offline_hdr = (root / "target/i386/latx/include/sys-aot-offline.h").read_text()
assert "#ifdef CONFIG_SOFTMMU" in offline_hdr
arith_src = (root / "target/i386/latx/translator/sys-tr-arith.c").read_text()
for fn_name in ("latxs_translate_div", "latxs_translate_idiv"):
    fn_body = extract_fn(arith_src, fn_name)
    assert "#ifdef CONFIG_SOFTMMU" in fn_body and "aot_offline_div_exception" in fn_body

save_src = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()
opt_fn = extract_fn(save_src, "aot_optimize_saved_user_aot")
assert "CONFIG_SOFTMMU" in opt_fn

# 2. Compile and run hamt_native_recover from native-recovery.inc both WITH and
#    WITHOUT CONFIG_LATX_OPT_NOFP (where CPUX86State has mode_fpu instead of nofp).
recover_inc = (root / "target/i386/hamt/native-recovery.inc").read_text()
recover_c = (
    r"""
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define error_report(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#include "latx-native-recover.h"

#define CC_C 0x0001
#define CC_P 0x0004
#define CC_A 0x0010
#define CC_Z 0x0040
#define CC_S 0x0080
#define CC_O 0x0800
#define CC_OP_EFLAGS 1

typedef struct { int cpu_index; } CPUState;
typedef struct { uint8_t _b_ZMMReg[64]; } ZMMReg;
typedef struct { struct { uint64_t low; uint16_t high; } d; } FPReg;

typedef struct CPUArchState {
    uint64_t regs[16];
    uint64_t eip;
    uint32_t cc_op;
    uint64_t cc_src;
    uint64_t cc_dst;
    ZMMReg xmm_regs[16];
    FPReg fpregs[8];
#ifdef CONFIG_LATX_OPT_NOFP
    int nofp;
#else
    bool mode_fpu;
#endif
    uint32_t fpstt;
    bool native_recovered;
} CPUX86State;

typedef struct TranslationBlock TranslationBlock;
typedef struct {
    TranslationBlock *tb;
    uint32_t *entry;
    uint32_t entry_bytes;
    const void *recover;
    uint32_t *core;
} AOTNativeRecord;
typedef struct {
    uint32_t count;
    AOTNativeRecord records[1];
} AOTNativeTUInfo;
typedef struct { AOTNativeTUInfo *native; } TranslationUnitPages;
struct TranslationBlock {
    uint64_t pc;
    TranslationUnitPages *pages;
};
#define TB_TU_PAGES(tb) ((tb)->pages)

static CPUState cpu_singleton = { .cpu_index = 0 };
static CPUState *env_cpu(CPUX86State *env) { (void)env; return &cpu_singleton; }
static bool qemu_tcg_mttcg_enabled(void) { return false; }
static bool aot_tu_enabled(void) { return true; }
static bool option_aot_load = true;
static bool option_soft_fpu = true;
__attribute__((unused)) static int option_soft_fpu_fast = 1;
static TranslationBlock *current_lookup_tb;
static TranslationBlock *tcg_tb_lookup(uintptr_t epc) { (void)epc; return current_lookup_tb; }

static uint8_t fake_frame[4096] __attribute__((aligned(64)));
static void *hamt_data_area[1] = { fake_frame };
static uint8_t fake_private_stack[4096] __attribute__((aligned(64)));

static void latxs_ras_tp_save(CPUX86State *env, uint64_t *frame) { (void)env; (void)frame; }
static uint32_t lsfpu_get_ftop(void) { return 0; }
static void leave_lsfpu_mode(void) {}
static uint32_t written_fcsr;
static void __write_native_fcsr0(uint32_t v) { written_fcsr = v; }
#ifdef CONFIG_LATX_OPT_EFLAGS
static uint64_t written_lbt_flags;
static void __lbt_mtflag_all(uint64_t v) { written_lbt_flags = v; }
#endif
"""
    + "\n"
    + extract_fn(recover_inc, "hamt_native_fault_record")
    + "\n"
    + extract_fn(recover_inc, "hamt_native_recover")
    + r"""
int main(void)
{
    uint32_t blob_words[256];
    memset(blob_words, 0, sizeof(blob_words));
    blob_words[0] = 0x28c00085U;
    blob_words[1] = 0x4c000020U;
    uint8_t *raw = (uint8_t *)blob_words;
    const uint32_t hdr_off = 8;
    const uint32_t table_bytes =
        sizeof(NativeRecoverHeader) + sizeof(NativeRecoverPoint) +
        2 * sizeof(NativeRecoverLocation);
    const uint32_t total_bytes = hdr_off + table_bytes + sizeof(NativeRecoverFooter);

    NativeRecoverHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = NATIVE_RECOVER_MAGIC;
    hdr.bytes = table_bytes;
    hdr.code_bytes = 8;
    hdr.points = 1;
    hdr.state_bytes = 152;
    hdr.frame_bytes = 1024;
    hdr.saved_xr_mask = (1u << 0) | (1u << 3);
    hdr.saved_xr_offsets[0] = 160;
    hdr.saved_xr_offsets[3] = 192;

    NativeRecoverPoint pt = {
        .native_offset = 0,
        .guest_offset = 12,
        .locations = 2,
        .flags_mask = 0,
        .memory_bytes = 8,
        .instruction = blob_words[0],
    };
    NativeRecoverLocation locs[2] = {
        { .field = 36, .kind = 0, .reg = 22, .bytes = 8 },
        { .field = 37, .kind = 0, .reg = 4, .bytes = 8 },
    };
    NativeRecoverFooter ftr = {
        .magic = NATIVE_RECOVER_FOOTER,
        .offset = hdr_off,
        .bytes = table_bytes,
    };
    size_t pos = hdr_off;
    memcpy(raw + pos, &hdr, sizeof(hdr));
    pos += sizeof(hdr);
    memcpy(raw + pos, &pt, sizeof(pt));
    pos += sizeof(pt);
    memcpy(raw + pos, locs, sizeof(locs));
    pos += sizeof(locs);
    memcpy(raw + pos, &ftr, sizeof(ftr));

    bool recognized = false;
    const NativeRecoverHeader *parsed =
        native_recover_parse(blob_words, total_bytes, &recognized);
    assert(recognized && parsed != NULL);

    AOTNativeTUInfo info = {
        .count = 1,
        .records = {{
            .entry = blob_words,
            .entry_bytes = total_bytes,
            .recover = parsed,
            .core = blob_words,
        }},
    };
    TranslationUnitPages pages = { .native = &info };
    TranslationBlock tb = { .pc = 0x401000, .pages = &pages };
    info.records[0].tb = &tb;
    current_lookup_tb = &tb;

    uint64_t *frame64 = (uint64_t *)fake_frame;
    frame64[22] = UINT64_C(0x9000000000000000) | (uintptr_t)fake_private_stack;
    frame64[4] = 0x7fff1000ULL;
    for (unsigned i = 0; i < 8; i++) {
        uint64_t v = UINT64_C(0xfeed000000000000) + i;
        memcpy(fake_frame + NATIVE_XR_OFFSET + i * 32, &v, 8);
    }
    uint64_t spilled0 = 0x1111222233334444ULL;
    uint64_t spilled3 = 0x5555666677778888ULL;
    uint64_t anchor = 0x401000ULL;
    memcpy(fake_private_stack + 160, &spilled0, 8);
    memcpy(fake_private_stack + 192, &spilled3, 8);
    memcpy(fake_private_stack + 136, &anchor, 8);

    CPUX86State env;
    memset(&env, 0, sizeof(env));
    for (unsigned i = 0; i < 8; i++) {
        env.fpregs[i].d.low = 0xdeadbeef00000000ULL + i;
    }
#ifdef CONFIG_LATX_OPT_NOFP
    env.nofp = 1;
    assert(hamt_native_recover(&env, (uintptr_t)blob_words, 0x7fff1000ULL));
    assert(env.nofp == 0);
    assert(env.fpregs[0].d.low == spilled0);
    assert(env.fpregs[3].d.low == spilled3);
    assert(env.fpregs[1].d.low == UINT64_C(0xfeed000000000001));

    env.fpregs[0].d.low = 0x9999ULL;
    env.nofp = 0;
    assert(hamt_native_recover(&env, (uintptr_t)blob_words, 0x7fff1000ULL));
    assert(env.fpregs[0].d.low == 0x9999ULL);
#else
    /* Non-NOFP mode: physical $f0-$f7 are live when !option_soft_fpu_fast || !mode_fpu. */
    option_soft_fpu_fast = 1;
    env.mode_fpu = false;
    assert(hamt_native_recover(&env, (uintptr_t)blob_words, 0x7fff1000ULL));
    assert(env.fpregs[0].d.low == spilled0);
    assert(env.fpregs[3].d.low == spilled3);
    assert(env.fpregs[2].d.low == UINT64_C(0xfeed000000000002));

    env.fpregs[0].d.low = 0xaaaaULL;
    env.mode_fpu = true;
    assert(hamt_native_recover(&env, (uintptr_t)blob_words, 0x7fff1000ULL));
    assert(env.fpregs[0].d.low == 0xaaaaULL);
#endif
    return 0;
}
"""
)

# 3. Compile and run gen_latxs_sc_prologue + ras_tp_pop to verify paired
#    aot_native_outer_sp save/restore across option_rastoptp on/off.
static_src = (root / "target/i386/latx/translator/sys-static-codes.c").read_text()
ras_src = (root / "target/i386/latx/optimization/sys-ras.c").read_text()

outer_sp_c = (
    r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define g_assert(c) assert(c)
#define UNUSED __attribute__((unused))
#define LATX_RAS_TP_SIZE 512

typedef uintptr_t ADDR;
typedef struct { int id; } IR2_OPND;
typedef struct { uint64_t pc, ptr; } LatxRasTPEntry;
typedef struct {
    LatxRasTPEntry sentinel;
    LatxRasTPEntry stack[LATX_RAS_TP_SIZE];
    uint32_t top;
    uint32_t reset_pending;
    uint64_t native_tp;
} LatxRasTPState;

typedef struct {
    LatxRasTPState ras_tp;
    uintptr_t aot_native_outer_sp;
} CPUX86State;
#define RAS_TP_OFF(field) ((int)offsetof(CPUX86State, ras_tp.field))

typedef struct { int ir2_asm_nr; } TRANSLATION_DATA;
typedef struct { TRANSLATION_DATA *tr_data; } ENV;
static TRANSLATION_DATA td_state;
static ENV env_state = { .tr_data = &td_state };
static ENV *lsenv = &env_state;

enum {
    LISA_ADDI_D = 1,
    LISA_ST_D,
    LISA_LD_D,
    LISA_LD_BU,
    LISA_OR,
    LISA_BNE,
    LISA_BEQ,
    LISA_B,
    LISA_JIRL,
    LISA_STPTR_W,
    LISA_STPTR_D,
    LISA_LDPTR_D,
    LISA_ALSL_D,
    IR2_OPND_GPR = 0,
};

static bool option_rastoptp = true;
static bool option_soft_fpu = true;
UNUSED static bool option_soft_fpu_fast = false;
static bool aot_tu_enabled(void) { return true; }
static bool hamt_enabled(void) { return true; }
static bool intb_link_enable(void) { return false; }
UNUSED static int lsenv_offset_of_mode_fpu(ENV *e) { (void)e; return 0; }
#define GET_SC_TABLE(mtcc, field) ((void)(mtcc), (uintptr_t)0x1000)
static inline int64_t sextract64(uint64_t v, int s, int l)
{
    return ((int64_t)(v << (64 - l - s))) >> (64 - l);
}

typedef struct { int op, rd, rj, imm; } Insn;
static Insn insns[256];
static unsigned n_insns;
static void rec(int op, int rd, int rj, int imm)
{
    assert(n_insns < 256);
    insns[n_insns++] = (Insn){ op, rd, rj, imm };
    td_state.ir2_asm_nr++;
}

static IR2_OPND la_inv = { -1 }, la_zero = { 0 }, la_ra = { 1 }, la_sp = { 3 },
                la_a0 = { 4 }, la_a1 = { 5 }, la_fp = { 22 }, la_env = { 23 };
static IR2_OPND la_new_gpr(int id) { return (IR2_OPND){ id }; }
UNUSED static IR2_OPND la_new_label(void) { static int l = 100; return (IR2_OPND){ ++l }; }
static IR2_OPND latxs_ir2_opnd_new_label(void) { static int l = 200; return (IR2_OPND){ ++l }; }
UNUSED static IR2_OPND la_new_itmp(void) { return (IR2_OPND){ 12 }; }
UNUSED static void la_tmp_free(IR2_OPND t) { (void)t; }
static IR2_OPND latxs_ir2_opnd_new(int k, int id) { return (IR2_OPND){ k + id }; }
static IR2_OPND latxs_ra_alloc_itemp(void) { static int t = 12; return (IR2_OPND){ t++ }; }
static void latxs_ra_free_temp(IR2_OPND *t) { (void)t; }
UNUSED static void la_out1(int op, const IR2_OPND *rd) { rec(op, rd->id, 0, 0); }
static void la_out2i(int op, const IR2_OPND *rd, const IR2_OPND *rj, int imm) { rec(op, rd->id, rj->id, imm); }
static void la_out3(int op, const IR2_OPND *rd, const IR2_OPND *rj, const IR2_OPND *rk) { rec(op, rd->id, rj->id, rk->id); }
static void la_out3i(int op, const IR2_OPND *rd, const IR2_OPND *rj, const IR2_OPND *rk, int imm) { (void)rk; rec(op, rd->id, rj->id, imm); }
static void la_outa(int op, int64_t off) { rec(op, 0, 0, (int)off); }
static void la_out_nop(void) {}
static void la_label(IR2_OPND l) { (void)l; }

static void latxs_tr_begin(void *p) { (void)p; n_insns = 0; td_state.ir2_asm_nr = 0; }
static int latxs_tr_end(void *p) { (void)p; return (int)n_insns; }
static void latxs_tr_dmw_in_fix_mem(const IR2_OPND *r) { (void)r; }
static void latxs_tr_dmw_out_fix_mem(const IR2_OPND *r) { (void)r; }
static void latxs_tr_dmw_in_fix_asid_tmp(const IR2_OPND *r) { (void)r; }
static void latxs_tr_load_registers_from_env(uint32_t g, uint8_t f, int t, uint32_t x, uint8_t v)
{ (void)g; (void)f; (void)t; (void)x; (void)v; }
UNUSED static void latxs_tr_load_fprs_from_env(uint8_t f, int t) { (void)f; (void)t; }
UNUSED static void latxs_tr_load_eflags(void) {}
static void latxs_ras_tp_leave_c(void) {}
static void latxs_tr_gen_intb_sync_gpc(void) {}
static void ras_tp_clear(void) {}
"""
    + "\n"
    + extract_fn(static_src, "aot_native_outer_sp_enabled")
    + "\n"
    + extract_fn(static_src, "aot_native_load_outer_sp")
    + "\n"
    + extract_fn(static_src, "gen_latxs_ras_tp_fallback")
    + "\n"
    + extract_fn(static_src, "gen_latxs_sc_prologue")
    + "\n"
    + extract_fn(ras_src, "ras_tp_pop")
    + r"""
static bool has_insn(int op, int rd, int rj, int imm)
{
    for (unsigned i = 0; i < n_insns; i++) {
        if (insns[i].op == op && insns[i].rd == rd &&
            insns[i].rj == rj && insns[i].imm == imm) {
            return true;
        }
    }
    return false;
}

int main(void)
{
    const int outer_imm = (int)(offsetof(CPUX86State, aot_native_outer_sp) >> 2);
    IR2_OPND ret_pc = { 8 };

    /* With option_rastoptp = true, prologue saves $sp and ras_tp_pop / fallback load it. */
    option_rastoptp = true;
    gen_latxs_sc_prologue((void *)0x1000);
    assert(has_insn(LISA_STPTR_D, 3, 23, outer_imm));

    n_insns = 0;
    assert(ras_tp_pop(&ret_pc));
    assert(has_insn(LISA_LDPTR_D, 3, 23, outer_imm));

    gen_latxs_ras_tp_fallback((void *)0x1000, 0);
    assert(has_insn(LISA_LDPTR_D, 3, 23, outer_imm));

    /* With option_rastoptp = false, neither prologue nor ras_tp_pop touches outer_sp. */
    option_rastoptp = false;
    gen_latxs_sc_prologue((void *)0x1000);
    assert(!has_insn(LISA_STPTR_D, 3, 23, outer_imm));

    n_insns = 0;
    assert(ras_tp_pop(&ret_pc));
    assert(!has_insn(LISA_LDPTR_D, 3, 23, outer_imm));
    return 0;
}
"""
)

with tempfile.TemporaryDirectory(prefix="lats-aot-guards-") as tmp:
    tdir = Path(tmp)
    rec_src = tdir / "recover.c"
    rec_src.write_text(recover_c)
    for flags in (
        ["-DCONFIG_LATX_OPT_NOFP", "-DCONFIG_LATX_OPT_EFLAGS"],
        ["-UCONFIG_LATX_OPT_NOFP", "-DCONFIG_LATX_OPT_EFLAGS"],
        ["-UCONFIG_LATX_OPT_NOFP", "-UCONFIG_LATX_OPT_EFLAGS"],
    ):
        exe = tdir / "recover"
        subprocess.run(
            [
                "cc",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{root / 'include'}",
                f"-I{root / 'target/i386/latx/include'}",
                *flags,
                str(rec_src),
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True)

    sp_src = tdir / "outer_sp.c"
    sp_src.write_text(outer_sp_c)
    for flags in (["-DCONFIG_LATX_OPT_NOFP"], ["-UCONFIG_LATX_OPT_NOFP"]):
        exe = tdir / "outer_sp"
        subprocess.run(
            ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", *flags, str(sp_src), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)

print("PASS: AOT SOFTMMU/NOFP build guards and paired aot_native_outer_sp verified")
