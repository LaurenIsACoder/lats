#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify x87/system AOT helpers (FWAIT/FNOP #NM, flush/reset exits, FCSR invariant) and HAMT RMW detection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
x87_src = (root / "target/i386/latx/sbt/sys-aot-x87.c").read_text()
system_src = (root / "target/i386/latx/sbt/sys-aot-system.c").read_text()
recovery_src = (root / "target/i386/hamt/native-recovery.inc").read_text()


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


program = (
    r"""
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lats/aot-x87.h"
#include "lats/aot-system.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), "build bug")
#define g_assert(c) assert(c)
#define g_assert_not_reached() abort()
#define qatomic_read(p) (*(p))

enum {
    R_EAX = 0,
    HF_MP_MASK = 1 << 1,
    HF_EM_MASK = 1 << 2,
    HF_TS_MASK = 1 << 3,
    HF_CS64_MASK = 1 << 15,
    EXCP07_PREX = 7,
    CC_O = 0x800,
    CC_S = 0x80,
    CC_Z = 0x40,
    CC_A = 0x10,
    CC_OP_EFLAGS = 1,
    TARGET_PAGE_SIZE = 4096,
    TARGET_PAGE_MASK = -4096,
};

typedef struct {
    uint64_t q[2];
} ZMMReg;
#define ZMM_Q(i) q[i]

typedef struct CPUX86State {
    uint64_t eip;
    uint64_t regs[16];
    ZMMReg xmm_regs[16];
    uint32_t hflags;
    uint32_t cc_src;
    uint32_t cc_op;
    unsigned is_fcsr_simd;
    bool native_recovered;
    unsigned mtcc;
    struct {
        uint32_t reset_pending;
    } ras_tp;
} CPUX86State;

typedef struct {
    CPUX86State *env;
} CPUState;

static CPUState cpu_wrapper;
static CPUState *env_cpu(CPUX86State *env)
{
    cpu_wrapper.env = env;
    return &cpu_wrapper;
}
static unsigned curr_mtcc(CPUX86State *env) { return env->mtcc; }

static struct {
    unsigned tb_phys_invalidate_count;
    unsigned tb_flush_count;
    unsigned tb_flush_count_mtcc[4];
} tb_ctx;

static bool option_soft_fpu = true;
static jmp_buf exit_jmp;
static int exit_reason; /* 1 = exception, 2 = loop_exit_noexc */
static int last_exception;
static unsigned simd_at_exit;

static void __attribute__((noreturn)) raise_exception(CPUX86State *env, int excp)
{
    last_exception = excp;
    simd_at_exit = env->is_fcsr_simd;
    exit_reason = 1;
    longjmp(exit_jmp, 1);
}

static void __attribute__((noreturn)) cpu_loop_exit_noexc(CPUState *cpu)
{
    simd_at_exit = cpu->env->is_fcsr_simd;
    exit_reason = 2;
    longjmp(exit_jmp, 2);
}

/* Hookable side effects during helper calls. */
static void (*on_helper_call)(CPUX86State *env);
static void trigger_hook(CPUX86State *env)
{
    if (on_helper_call) {
        on_helper_call(env);
    }
}

static void helper_fldt_ST0(CPUX86State *e, uint64_t a) { (void)a; trigger_hook(e); }
static void helper_fstt_ST0(CPUX86State *e, uint64_t a) { (void)a; trigger_hook(e); }
static void helper_fpop(CPUX86State *e) { (void)e; }
static void helper_fpush(CPUX86State *e) { (void)e; }
static void helper_fmov_ST0_STN(CPUX86State *e, unsigned r) { (void)r; trigger_hook(e); }
static void helper_fmov_STN_ST0(CPUX86State *e, unsigned r) { (void)r; trigger_hook(e); }
static void helper_fabs_ST0(CPUX86State *e) { trigger_hook(e); }
static void helper_fxchg_ST0_STN(CPUX86State *e, unsigned r) { (void)r; trigger_hook(e); }
static void helper_fxam_ST0(CPUX86State *e) { trigger_hook(e); }
static void helper_fmov_FT0_STN(CPUX86State *e, unsigned r) { (void)e; (void)r; }
static void helper_fcomi_ST0_FT0(CPUX86State *e) { trigger_hook(e); }
static void helper_fucomi_ST0_FT0(CPUX86State *e) { trigger_hook(e); }
static uint16_t helper_fnstsw(CPUX86State *e) { trigger_hook(e); return 0x1234; }
static uint16_t helper_fnstcw(CPUX86State *e) { trigger_hook(e); return 0x037f; }
static void cpu_stw_data(CPUX86State *e, uint64_t a, uint16_t v) { (void)a; (void)v; trigger_hook(e); }
static void cpu_stl_data(CPUX86State *e, uint64_t a, uint32_t v) { (void)a; (void)v; trigger_hook(e); }
static void cpu_stq_data(CPUX86State *e, uint64_t a, uint64_t v) { (void)a; (void)v; trigger_hook(e); }
static uint64_t cpu_ldq_data(CPUX86State *e, uint64_t a) { (void)e; (void)a; return 0; }
static void helper_fprem(CPUX86State *e) { trigger_hook(e); }
static void helper_fprem1(CPUX86State *e) { trigger_hook(e); }
static void helper_fxtract(CPUX86State *e) { trigger_hook(e); }
static void helper_fsin(CPUX86State *e) { trigger_hook(e); }
static void helper_fcos(CPUX86State *e) { trigger_hook(e); }
static void helper_fpatan(CPUX86State *e) { trigger_hook(e); }
static void helper_fsincos(CPUX86State *e) { trigger_hook(e); }
static void helper_fptan(CPUX86State *e) { trigger_hook(e); }
static void helper_fyl2x(CPUX86State *e) { trigger_hook(e); }
static void helper_fyl2xp1(CPUX86State *e) { trigger_hook(e); }
static void helper_f2xm1(CPUX86State *e) { trigger_hook(e); }
static void helper_fbld_ST0(CPUX86State *e, uint64_t a) { (void)a; trigger_hook(e); }
static void helper_fbst_ST0(CPUX86State *e, uint64_t a) { (void)a; trigger_hook(e); }
static void helper_fildll_ST0(CPUX86State *e, uint64_t a) { (void)a; trigger_hook(e); }
static int16_t helper_fist_ST0(CPUX86State *e) { (void)e; return 0; }
static int16_t helper_fistt_ST0(CPUX86State *e) { (void)e; return 0; }
static int32_t helper_fistl_ST0(CPUX86State *e) { (void)e; return 0; }
static int32_t helper_fisttl_ST0(CPUX86State *e) { (void)e; return 0; }
static int64_t helper_fistll_ST0(CPUX86State *e) { (void)e; return 0; }
static int64_t helper_fisttll_ST0(CPUX86State *e) { (void)e; return 0; }
static void helper_fwait(CPUX86State *e) { trigger_hook(e); }

static void helper_pcmpistri_xmm(CPUX86State *e, void *d, void *s, uint32_t c)
{ (void)d; (void)s; (void)c; trigger_hook(e); }
static void helper_pcmpistrm_xmm(CPUX86State *e, void *d, void *s, uint32_t c)
{ (void)d; (void)s; (void)c; trigger_hook(e); }
static void helper_pcmpestri_xmm(CPUX86State *e, void *d, void *s, uint32_t c)
{ (void)d; (void)s; (void)c; trigger_hook(e); }
static void helper_pcmpestrm_xmm(CPUX86State *e, void *d, void *s, uint32_t c)
{ (void)d; (void)s; (void)c; trigger_hook(e); }
static uint64_t helper_lar(CPUX86State *e, uint64_t a) { (void)a; trigger_hook(e); return 0; }
static uint64_t helper_lsl(CPUX86State *e, uint64_t a) { (void)a; trigger_hook(e); return 0; }
static void helper_verr(CPUX86State *e, uint64_t a) { (void)a; trigger_hook(e); }
static void helper_verw(CPUX86State *e, uint64_t a) { (void)a; trigger_hook(e); }
static void helper_latx_cmpxchg8b_unlocked(CPUX86State *e, uint64_t a)
{ (void)a; trigger_hook(e); }
static void helper_latx_cmpxchg16b_unlocked(CPUX86State *e, uint64_t a)
{ (void)a; trigger_hook(e); }

/* Stubs for hamt_native_guest_rmw */
enum {
    X86_OP_INVALID = 0,
    X86_OP_REG = 1,
    X86_OP_MEM = 2,
    X86_INS_ADD = 1, X86_INS_ADC, X86_INS_SUB, X86_INS_SBB,
    X86_INS_AND, X86_INS_OR, X86_INS_XOR,
    X86_INS_INC, X86_INS_DEC,
    X86_INS_NEG, X86_INS_NOT,
    X86_INS_SHL, X86_INS_SAL, X86_INS_SHR, X86_INS_SAR,
    X86_INS_ROL, X86_INS_ROR, X86_INS_RCL, X86_INS_RCR,
    X86_INS_SHLD, X86_INS_SHRD,
    X86_INS_BTC, X86_INS_BTR, X86_INS_BTS,
    X86_INS_XADD,
    X86_INS_CMPXCHG, X86_INS_CMPXCHG8B, X86_INS_CMPXCHG16B,
    X86_INS_XCHG,
    X86_INS_CMP, X86_INS_TEST, X86_INS_BT, X86_INS_MOV, X86_INS_PUSH,
};

typedef struct {
    struct {
        uint8_t op_count;
        struct { int type; } operands[4];
    } x86;
} cs_detail;

typedef struct {
    unsigned id;
    cs_detail *detail;
} cs_insn;

static int handle64;
static unsigned mock_insn_id;
static uint8_t mock_op_count;
static int mock_op0_type, mock_op1_type;

static int cpu_memory_rw_debug(CPUState *cpu, uint64_t addr, void *buf, size_t len, bool is_write)
{
    (void)cpu; (void)addr; (void)is_write;
    memset(buf, 0x90, len);
    return 0;
}
static void error_report(const char *msg) { fprintf(stderr, "%s\n", msg); }
static size_t cs_disasm(int h, const uint8_t *code, size_t size, uint64_t addr,
                        size_t count, cs_insn **insn_out, int x, void *storage)
{
    (void)h; (void)code; (void)size; (void)addr; (void)count; (void)x;
    struct { cs_insn insn; cs_detail detail; } *buf = storage;
    memset(buf, 0, sizeof(*buf));
    buf->insn.id = mock_insn_id;
    buf->insn.detail = &buf->detail;
    buf->detail.x86.op_count = mock_op_count;
    buf->detail.x86.operands[0].type = mock_op0_type;
    buf->detail.x86.operands[1].type = mock_op1_type;
    *insn_out = &buf->insn;
    return 1;
}
"""
    + "\n"
    + extract_fn(x87_src, "latx_helper_aot_x87")
    + "\n"
    + extract_fn(system_src, "latx_helper_aot_system")
    + "\n"
    + extract_fn(recovery_src, "hamt_native_guest_rmw")
    + r"""
static void bump_partial_flush(CPUX86State *env)
{
    tb_ctx.tb_flush_count_mtcc[env->mtcc]++;
}
static void set_ras_reset(CPUX86State *env)
{
    env->ras_tp.reset_pending = 1;
}
static void bump_phys_invalidate(CPUX86State *env)
{
    (void)env;
    tb_ctx.tb_phys_invalidate_count++;
}

static bool check_rmw(unsigned id, uint8_t op_count, int op0, int op1)
{
    CPUX86State env = { .eip = 0x1000, .hflags = HF_CS64_MASK };
    mock_insn_id = id;
    mock_op_count = op_count;
    mock_op0_type = op0;
    mock_op1_type = op1;
    return hamt_native_guest_rmw(&env);
}

int main(void)
{
    /* 1. aot-x87.h: FWAIT (9B, prefixed 66 9B / 48 9B) vs FNOP (D9 D0) decoding & validation. */
    unsigned fwait1 = lats_aot_x87_decode((uint8_t[]){ 0x9b }, 1);
    unsigned fwait2 = lats_aot_x87_decode((uint8_t[]){ 0x66, 0x9b }, 2);
    unsigned fwait3 = lats_aot_x87_decode((uint8_t[]){ 0x48, 0x9b }, 2);
    unsigned fnop = lats_aot_x87_decode((uint8_t[]){ 0xd9, 0xd0 }, 2);
    unsigned fnop_p = lats_aot_x87_decode((uint8_t[]){ 0x66, 0xd9, 0xd0 }, 3);
    assert(lats_aot_x87_valid(fwait1) && LATS_X87_KIND(fwait1) == LATS_X87_FWAIT && LATS_X87_LENGTH(fwait1) == 1);
    assert(lats_aot_x87_valid(fwait2) && LATS_X87_KIND(fwait2) == LATS_X87_FWAIT && LATS_X87_LENGTH(fwait2) == 2);
    assert(lats_aot_x87_valid(fwait3) && LATS_X87_KIND(fwait3) == LATS_X87_FWAIT && LATS_X87_LENGTH(fwait3) == 2);
    assert(lats_aot_x87_valid(fnop) && LATS_X87_KIND(fnop) == LATS_X87_FNOP && LATS_X87_LENGTH(fnop) == 2);
    assert(lats_aot_x87_valid(fnop_p) && LATS_X87_KIND(fnop_p) == LATS_X87_FNOP && LATS_X87_LENGTH(fnop_p) == 3);

    /* 2. FWAIT raises #NM ONLY when CR0.MP && CR0.TS; FNOP raises #NM on CR0.EM || CR0.TS. */
    CPUX86State env;
    on_helper_call = NULL;

    /* FWAIT with EM=1, TS=0 -> NO #NM! */
    memset(&env, 0, sizeof(env));
    env.hflags = HF_EM_MASK;
    env.is_fcsr_simd = 1;
    exit_reason = 0;
    if (!setjmp(exit_jmp)) {
        latx_helper_aot_x87(&env, fwait1, 0);
    }
    assert(exit_reason == 0 && env.is_fcsr_simd == 1);

    /* FWAIT with MP=0, TS=1 -> NO #NM! */
    memset(&env, 0, sizeof(env));
    env.hflags = HF_TS_MASK;
    exit_reason = 0;
    if (!setjmp(exit_jmp)) {
        latx_helper_aot_x87(&env, fwait1, 0);
    }
    assert(exit_reason == 0);

    /* FWAIT with MP=1, TS=1 -> raises #NM with is_fcsr_simd == 0! */
    memset(&env, 0, sizeof(env));
    env.hflags = HF_MP_MASK | HF_TS_MASK;
    env.is_fcsr_simd = 1;
    exit_reason = 0;
    if (!setjmp(exit_jmp)) {
        latx_helper_aot_x87(&env, fwait1, 0);
    }
    assert(exit_reason == 1 && last_exception == EXCP07_PREX && simd_at_exit == 0);

    /* FNOP with EM=1, TS=0 -> raises #NM! */
    memset(&env, 0, sizeof(env));
    env.hflags = HF_EM_MASK;
    env.is_fcsr_simd = 1;
    exit_reason = 0;
    if (!setjmp(exit_jmp)) {
        latx_helper_aot_x87(&env, fnop, 0);
    }
    assert(exit_reason == 1 && last_exception == EXCP07_PREX && simd_at_exit == 0);

    /* FNOP with MP=0, TS=1 -> raises #NM! */
    memset(&env, 0, sizeof(env));
    env.hflags = HF_TS_MASK;
    exit_reason = 0;
    if (!setjmp(exit_jmp)) {
        latx_helper_aot_x87(&env, fnop, 0);
    }
    assert(exit_reason == 1 && last_exception == EXCP07_PREX);

    /* 3. Partial MTCC flush & RAS reset_pending exit with is_fcsr_simd == 0 and advanced EIP. */
    for (int mode = 0; mode < 2; mode++) {
        memset(&env, 0, sizeof(env));
        env.eip = 0x4000;
        env.mtcc = 2;
        env.is_fcsr_simd = 1;
        on_helper_call = mode == 0 ? bump_partial_flush : set_ras_reset;
        exit_reason = 0;
        if (!setjmp(exit_jmp)) {
            latx_helper_aot_x87(&env, fnop, 0);
        }
        assert(exit_reason == 2 && simd_at_exit == 0 && env.eip == 0x4002);
    }

    /* 4. latx_helper_aot_system: partial MTCC flush, RAS reset_pending, and write invalidation
     *    all exit via cpu_loop_exit_noexc BEFORE restoring is_fcsr_simd to 1. */
    unsigned verr_desc = lats_aot_system_decode((uint8_t[]){ 0x0f, 0x00, 0xe0 }, 3);
    unsigned cmpxchg16b_desc = lats_aot_system_decode((uint8_t[]){ 0x48, 0x0f, 0xc7, 0x08 }, 4);
    assert(lats_aot_system_valid(verr_desc) && LATS_SYSTEM_KIND(verr_desc) == LATS_SYSTEM_VERR);
    assert(lats_aot_system_valid(cmpxchg16b_desc) && LATS_SYSTEM_KIND(cmpxchg16b_desc) == LATS_SYSTEM_CMPXCHG16B);
    for (int mode = 0; mode < 3; mode++) {
        memset(&env, 0, sizeof(env));
        env.eip = 0x8000;
        env.mtcc = 1;
        env.is_fcsr_simd = 1;
        unsigned desc = mode == 2 ? cmpxchg16b_desc : verr_desc;
        unsigned expected_len = mode == 2 ? 4 : 3;
        on_helper_call = mode == 0 ? bump_partial_flush
                       : mode == 1 ? set_ras_reset
                                   : bump_phys_invalidate;
        exit_reason = 0;
        if (!setjmp(exit_jmp)) {
            latx_helper_aot_system(&env, desc, 0x9000);
        }
        assert(exit_reason == 2 && simd_at_exit == 0 && env.eip == 0x8000 + expected_len);
    }

    /* 5. hamt_native_guest_rmw covers full x86 memory RMW set and excludes read-only ops. */
    const unsigned rmw_ops[] = {
        X86_INS_ADD, X86_INS_SUB, X86_INS_INC, X86_INS_DEC,
        X86_INS_ROL, X86_INS_ROR, X86_INS_RCL, X86_INS_RCR,
        X86_INS_SHLD, X86_INS_SHRD,
        X86_INS_BTC, X86_INS_BTR, X86_INS_BTS,
        X86_INS_XADD, X86_INS_CMPXCHG, X86_INS_CMPXCHG8B, X86_INS_CMPXCHG16B,
    };
    for (size_t i = 0; i < sizeof(rmw_ops) / sizeof(rmw_ops[0]); i++) {
        assert(check_rmw(rmw_ops[i], 2, X86_OP_MEM, X86_OP_REG));
        assert(!check_rmw(rmw_ops[i], 2, X86_OP_REG, X86_OP_MEM));
    }
    /* XCHG allows memory in either operand 0 or operand 1. */
    assert(check_rmw(X86_INS_XCHG, 2, X86_OP_MEM, X86_OP_REG));
    assert(check_rmw(X86_INS_XCHG, 2, X86_OP_REG, X86_OP_MEM));
    assert(!check_rmw(X86_INS_XCHG, 2, X86_OP_REG, X86_OP_REG));

    /* Read-only memory accesses remain non-write faults. */
    assert(!check_rmw(X86_INS_CMP, 2, X86_OP_MEM, X86_OP_REG));
    assert(!check_rmw(X86_INS_TEST, 2, X86_OP_MEM, X86_OP_REG));
    assert(!check_rmw(X86_INS_BT, 2, X86_OP_MEM, X86_OP_REG));
    assert(!check_rmw(X86_INS_MOV, 2, X86_OP_REG, X86_OP_MEM));
    assert(!check_rmw(X86_INS_PUSH, 1, X86_OP_MEM, X86_OP_INVALID));

    puts("PASS: x87/system AOT helpers and HAMT RMW recovery verified");
    return 0;
}
"""
)

with tempfile.TemporaryDirectory(prefix="lats-aot-helpers-rmw-") as folder:
    binary = Path(folder) / "check"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
         f"-I{root / 'include'}",
         "-x", "c", "-o", str(binary), "-"],
        input=program,
        text=True,
        check=True,
    )
    subprocess.run([str(binary)], check=True)
