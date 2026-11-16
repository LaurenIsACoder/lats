#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify inline helper prologues/epilogues, AOT helper stubs, and jump_cold preserve live $fcsr0 and FPR/NOFP MMX state."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
helper_src = (root / "target/i386/latx/translator/sys-helper.c").read_text()
static_src = (root / "target/i386/latx/translator/sys-static-codes.c").read_text()


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


# Never substitute empty stubs: sys-helper.c and sys-static-codes.c must compile
# cleanly without touching uninitialized sp+128 FCSR slots.
program = (
    r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CONFIG_LATX_TU 1
#define CONFIG_SOFTMMU 1
#define LATX_SYS_FCSR 1
#define IRQ_CHECK_ASYNC_NOTIFY 1
#define likely(x) (x)
#define unlikely(x) (x)
#define g_assert(c) assert(c)
#define UNUSED __attribute__((unused))

typedef uintptr_t ADDR;
typedef struct { int id; } IR2_OPND;
typedef struct {
    union {
        uint32_t data;
        struct {
            uint32_t sv_allgpr : 1;
            uint32_t sv_gpr : 1;
            uint32_t sv_fpr : 1;
            uint32_t sv_simd : 1;
            uint32_t sv_eflags : 1;
            uint32_t cvt_fp80 : 1;
        } cfg;
    };
} helper_cfg_t;

static helper_cfg_t default_helper_cfg = { .cfg.sv_allgpr = 1, .cfg.sv_eflags = 1 };
static helper_cfg_t nofp_helper_cfg = { .cfg.sv_gpr = 1, .cfg.sv_simd = 1, .cfg.sv_eflags = 1 };

typedef struct { int dummy; } TranslationBlock;
typedef struct {
    TranslationBlock *curr_tb;
    int ir2_asm_nr;
    uint32_t mtcc_id;
    uint64_t tcg_region_offset;
} TRANSLATION_DATA;
typedef struct { TRANSLATION_DATA *tr_data; } ENV;
static TRANSLATION_DATA td_state;
static ENV env_state = { .tr_data = &td_state };
static ENV *lsenv = &env_state;

typedef struct {
    struct { uint32_t top, reset_pending; } ras_tp;
    uint32_t nofp;
    struct { struct { uint64_t low, high; } d; } fpregs[8];
} CPUX86State;

typedef enum {
    AOT_HELPER_CPU,
    AOT_HELPER_X87,
    AOT_HELPER_ATOMIC,
    AOT_HELPER_SYSTEM,
} AotReturningHelper;

enum {
    LISA_SRLI_D = 1,
    LISA_BNEZ,
    LISA_ADDI_D,
    LISA_STPTR_W,
    LISA_STPTR_D,
    LISA_LDPTR_W,
    LISA_LDPTR_D,
    LISA_LD_BU,
    LISA_LD_W,
    LISA_LD_D,
    LISA_ST_W,
    LISA_ST_D,
    LISA_FLD_D,
    LISA_FST_D,
    LISA_BEQZ,
    LISA_BEQ,
    LISA_BNE,
    LISA_OR,
    LISA_ORI,
    LISA_JIRL,
    LISA_B,
    IR2_OPND_FPR = 100,
    SCS_PROLOGUE = 1,
    SCS_PROLOGUE_NOFP = 2,
    SCS_EPILOGUE = 3,
    SCS_EPILOGUE_NOFP = 4,
};

static bool option_soft_fpu = true;
UNUSED static bool option_soft_fpu_fast = false;
static bool option_soft_ras = true;
static int option_int_opt = IRQ_CHECK_ASYNC_NOTIFY;
static uintptr_t fake_return_target;
#define GET_SC_TABLE(mtcc, field) ((void)(mtcc), fake_return_target)
#define tb_sc_table_offset_jmp(tb, mtcc, field, roff, off, s, k) \
    ((void)(tb), (void)(mtcc), (void)(roff), (void)(off), (void)(s), (void)(k), abort())

static inline int64_t sextract64(uint64_t value, int start, int length)
{
    return ((int64_t)(value << (64 - length - start))) >> (64 - length);
}

static bool scs_enabled(void) { return true; }
static bool hamt_enabled(void) { return true; }
static bool hamt_mode_dmw(void) { return true; }
static bool aot_native_outer_sp_enabled(void) { return true; }
UNUSED static int lsenv_offset_of_mode_fpu(ENV *e) { (void)e; return 0; }
static int cmp_helper_cfg(helper_cfg_t a, helper_cfg_t b) { return a.data == b.data; }

/* Any attempt to touch hardware $fcsr0 or read uninitialized sp+128 fails immediately. */
UNUSED static void latxs_save_fcsr_cs_helper_prologue(IR2_OPND *t) { (void)t; abort(); }
UNUSED static void latxs_load_dbt_fcsr(IR2_OPND *t) { (void)t; abort(); }
UNUSED static void latxs_load_fcsr_cs_helper_epilogue(IR2_OPND *t) { (void)t; abort(); }

typedef struct {
    int op, rd, rj, imm;
} Emitted;

static Emitted emitted[512];
static unsigned emitted_count;
static int sp_delta;

static IR2_OPND la_inv = { -1 }, la_zero = { 0 }, la_ra = { 1 }, la_sp = { 3 },
                la_v0 = { 4 }, la_a0 = { 4 }, la_a1 = { 5 }, la_a2 = { 6 },
                la_fp = { 22 }, la_env = { 23 };
#define latxs_env_ir2_opnd la_env
static IR2_OPND la_new_gpr(int id) { return (IR2_OPND){ id }; }
static IR2_OPND la_new_label(void) { static int lbl = 1000; return (IR2_OPND){ ++lbl }; }
UNUSED static IR2_OPND la_new_itmp(void) { return (IR2_OPND){ 12 }; }
UNUSED static void la_tmp_free(IR2_OPND t) { (void)t; }
UNUSED static IR2_OPND latxs_ir2_opnd_new(int kind, int id) { return (IR2_OPND){ kind + id }; }
UNUSED static IR2_OPND latxs_ra_alloc_itemp(void) { return (IR2_OPND){ 12 }; }
UNUSED static IR2_OPND latxs_ra_alloc_dbt_arg1(void) { return (IR2_OPND){ 10 }; }
UNUSED static void latxs_ra_free_temp(IR2_OPND *t) { (void)t; }

static void record(int op, int rd, int rj, int imm)
{
    assert(emitted_count < sizeof(emitted) / sizeof(emitted[0]));
    /* No helper stub or epilogue may read or write the bogus sp+128 FCSR slot. */
    if ((op == LISA_LD_D || op == LISA_ST_D) && imm == 128) {
        abort();
    }
    if (op == LISA_ADDI_D && rd == 3 && rj == 3) {
        sp_delta += imm;
    }
    emitted[emitted_count++] = (Emitted){ op, rd, rj, imm };
    td_state.ir2_asm_nr++;
}

static void la_out1(int op, const IR2_OPND *rd) { record(op, rd->id, 0, 0); }
static void la_out2i(int op, const IR2_OPND *rd, const IR2_OPND *rj, int imm) { record(op, rd->id, rj->id, imm); }
static void la_out2(int op, const IR2_OPND *rd, const IR2_OPND *rj) { record(op, rd->id, rj->id, 0); }
static void la_out3(int op, const IR2_OPND *rd, const IR2_OPND *rj, const IR2_OPND *rk) { record(op, rd->id, rj->id, rk->id); }
static void la_outa(int op, int64_t off) { record(op, 0, 0, (int)off); }
static void la_imm(const IR2_OPND *rd, ADDR addr) { record(900, rd->id, 0, (int)(addr & 0xfff)); }
static void la_label(IR2_OPND lbl) { (void)lbl; }
static void la_label_name(IR2_OPND lbl, const char *name) { (void)lbl; (void)name; }

static void latxs_tr_begin(void *code_ptr)
{
    (void)code_ptr;
    emitted_count = 0;
    sp_delta = 0;
    td_state.ir2_asm_nr = 0;
}
static int latxs_tr_end(void *code_ptr) { (void)code_ptr; return td_state.ir2_asm_nr; }
static void aot_native_load_outer_sp(const IR2_OPND *dst) { record(901, dst->id, 23, 0); }
static void gen_aot_atomic_fast_path(void) {}
UNUSED static void latxs_gen_nofp_sync_fprs_to_env(void) { record(902, 0, 23, 0); }

static void latxs_ras_tp_enter_c(void) {}
static void latxs_ras_tp_leave_c(void) {}
static void latxs_tr_save_fprs_to_env(uint8_t m, int t);
static void latxs_tr_load_fprs_from_env(uint8_t m, int t);
static void latxs_tr_save_registers_to_env(uint32_t g, uint8_t f, int t, uint32_t x, uint8_t v)
{
    (void)g; (void)x; (void)v;
    if (f) {
        latxs_tr_save_fprs_to_env(f, t);
    }
}
static void latxs_tr_load_registers_from_env(uint32_t g, uint8_t f, int t, uint32_t x, uint8_t v)
{
    (void)g; (void)x; (void)v;
    if (f) {
        latxs_tr_load_fprs_from_env(f, t);
    }
}
static void latxs_tr_save_gprs_to_env(uint32_t m) { (void)m; }
static void latxs_tr_save_fprs_to_env(uint8_t m, int t)
{
    (void)m; (void)t;
#ifndef CONFIG_LATX_OPT_NOFP
    for (unsigned n = 0; n < 8; n++) {
        IR2_OPND reg = latxs_ir2_opnd_new(IR2_OPND_FPR, n);
        la_out2i(LISA_FST_D, &reg, &la_env,
                 (int)offsetof(CPUX86State, fpregs[n].d.low));
    }
#endif
}
static void latxs_tr_save_xmms_to_env(uint32_t m) { (void)m; }
static void latxs_tr_load_gprs_from_env(uint32_t m) { (void)m; }
static void latxs_tr_load_fprs_from_env(uint8_t m, int t)
{
    (void)m; (void)t;
#ifndef CONFIG_LATX_OPT_NOFP
    for (unsigned n = 0; n < 8; n++) {
        IR2_OPND reg = latxs_ir2_opnd_new(IR2_OPND_FPR, n);
        la_out2i(LISA_FLD_D, &reg, &la_env,
                 (int)offsetof(CPUX86State, fpregs[n].d.low));
    }
#endif
}
static void latxs_tr_load_xmms_from_env(uint32_t m) { (void)m; }
static void latxs_tr_save_eflags(void) {}
UNUSED static void latxs_tr_load_eflags(void) {}
static void latxs_tr_save_temp_register(void) {}
static void latxs_tr_restore_temp_register(void) {}
static void latxs_tr_cvt_fp64_to_80(void) {}
static void latxs_tr_cvt_fp80_to_64(void) {}
static void latxs_tr_dmw_out_fix_asid(IR2_OPND *t) { (void)t; }
static void latxs_tr_dmw_in_fix_asid_tmp(const IR2_OPND *r) { (void)r; }
static void latxs_tr_dmw_in_fix_mem(const IR2_OPND *r) { (void)r; }
static void latxs_tr_dmw_out_fix_mem(const IR2_OPND *r) { (void)r; }

static void helper_syscall(void) {}
static void latx_helper_aot_atomic_request(void) {}
static void latx_helper_aot_atomic(void) {}
static void latx_helper_aot_x87(void) {}
static void latx_helper_aot_system(void) {}
static void latx_helper_aot_cpu_query(void) {}
static void latx_helper_aot_ordinary_jump(void) {}

void latxs_tr_gen_call_to_helper_prologue_inline(helper_cfg_t cfg);
void latxs_tr_gen_call_to_helper_epilogue_inline(helper_cfg_t cfg);
static void gen_aot_returning_helper_body(AotReturningHelper kind);
"""
    + "\n"
    + extract_fn(helper_src, "gen_helper_prologue")
    + "\n"
    + extract_fn(helper_src, "gen_helper_epilogue")
    + "\n"
    + extract_fn(helper_src, "latxs_tr_gen_call_to_helper_prologue_inline")
    + "\n"
    + extract_fn(helper_src, "latxs_tr_gen_call_to_helper_epilogue_inline")
    + "\n"
    + extract_fn(static_src, "gen_latxs_aot_native_helper")
    + "\n"
    + extract_fn(static_src, "gen_aot_returning_helper_body")
    + "\n"
    + extract_fn(static_src, "gen_latxs_sc_epilogue")
    + r"""
static int find_insn_in_range(unsigned start, unsigned end,
                              int op, int rd, int rj, int imm)
{
    for (unsigned i = start; i < end && i < emitted_count; i++) {
        if (emitted[i].op == op &&
            (rd == -2 || emitted[i].rd == rd) &&
            (rj == -2 || emitted[i].rj == rj) &&
            (imm == -999999 || emitted[i].imm == imm)) {
            return (int)i;
        }
    }
    return -1;
}

static int find_insn(unsigned start, int op, int rd, int rj, int imm)
{
    return find_insn_in_range(start, emitted_count, op, rd, rj, imm);
}

static bool has_insn(int op, int rd, int rj, int imm)
{
    return find_insn(0, op, rd, rj, imm) >= 0;
}

static unsigned count_op_in_range(unsigned start, unsigned end, int op)
{
    unsigned count = 0;
    for (unsigned i = start; i < end && i < emitted_count; i++) {
        if (emitted[i].op == op) {
            count++;
        }
    }
    return count;
}

int main(void)
{
    uint32_t dummy_code[256];
    UNUSED const int nofp_off = (int)offsetof(CPUX86State, nofp);
    fake_return_target = (uintptr_t)(dummy_code + 128);

    /* 1. Syscall stub (atomic == false): balanced 16-byte frame, flushes $f0-$f7 / MMX
     * before helper_syscall (safe across #UD siglongjmp and C caller-saved clobber). */
    gen_latxs_aot_native_helper(dummy_code, 0, false);
    assert(sp_delta == 0);
    assert(has_insn(LISA_ADDI_D, 3, 3, -16));
    assert(has_insn(LISA_ADDI_D, 3, 3, 16));
    assert(has_insn(LISA_STPTR_D, 4, 3, 0));
    assert(has_insn(LISA_LDPTR_D, 5, 3, 0));
    {
        int sys_jirl = find_insn(0, LISA_JIRL, 1, 1, 0);
        assert(sys_jirl > 0);
        assert(count_op_in_range(0, (unsigned)sys_jirl, LISA_FST_D) == 8);
#ifdef CONFIG_LATX_OPT_NOFP
        int load_nofp = find_insn_in_range(0, (unsigned)sys_jirl, LISA_LD_W, -2, 23, nofp_off);
        int clear_nofp = find_insn_in_range(0, (unsigned)sys_jirl, LISA_ST_W, 0, 23, nofp_off);
        assert(load_nofp >= 0 && clear_nofp > load_nofp);
        assert(count_op_in_range((unsigned)sys_jirl + 1, emitted_count, LISA_FLD_D) == 0);
#else
        assert(count_op_in_range((unsigned)sys_jirl + 1, emitted_count, LISA_FLD_D) == 8);
#endif
    }

    /* 2. Atomic stub (atomic == true): verify BOTH terminal request path and returning path. */
    gen_latxs_aot_native_helper(dummy_code, 0, true);
    assert(has_insn(LISA_ADDI_D, 3, 3, -16));
    assert(has_insn(LISA_ADDI_D, 3, 3, -32));
    assert(has_insn(LISA_ADDI_D, 3, 3, 32));
    {
        int term_jirl = find_insn(0, LISA_JIRL, 1, 1, 0);
        assert(term_jirl > 0);
        int ret_jirl = find_insn((unsigned)term_jirl + 1, LISA_JIRL, 1, 1, 0);
        assert(ret_jirl > term_jirl);

        /* Terminal path: exact 8 FST_D (no duplicate save) and NOFP clear before JIRL. */
        assert(count_op_in_range(0, (unsigned)term_jirl, LISA_FST_D) == 8);
#ifdef CONFIG_LATX_OPT_NOFP
        int term_load = find_insn_in_range(0, (unsigned)term_jirl, LISA_LD_W, -2, 23, nofp_off);
        int term_clear = find_insn_in_range(0, (unsigned)term_jirl, LISA_ST_W, 0, 23, nofp_off);
        assert(term_load >= 0 && term_clear > term_load);

        int ret_save = find_insn_in_range((unsigned)term_jirl + 1, (unsigned)ret_jirl, LISA_ST_W, 4, 3, 24);
        int ret_clear = find_insn_in_range((unsigned)term_jirl + 1, (unsigned)ret_jirl, LISA_ST_W, 0, 23, nofp_off);
        int ret_reload = find_insn_in_range((unsigned)ret_jirl + 1, emitted_count, LISA_LD_W, 4, 3, 24);
        int ret_restore = find_insn_in_range((unsigned)ret_jirl + 1, emitted_count, LISA_ST_W, 4, 23, nofp_off);
        assert(ret_save > term_jirl && ret_clear > ret_save);
        assert(ret_reload > ret_jirl && ret_restore > ret_reload);
#endif
        assert(count_op_in_range((unsigned)term_jirl + 1, (unsigned)ret_jirl, LISA_FST_D) == 8);
        assert(count_op_in_range((unsigned)ret_jirl + 1, emitted_count, LISA_FLD_D) == 8);
    }

    /* 3. Returning helpers: balanced 32-byte frame, exact arg reloads, and NOFP MMX sync/restore. */
    for (unsigned kind = 0; kind < 4; kind++) {
        latxs_tr_begin(dummy_code);
        gen_aot_returning_helper_body((AotReturningHelper)kind);
        assert(sp_delta == 0);
        assert(has_insn(LISA_ADDI_D, 3, 3, -32));
        assert(has_insn(LISA_ADDI_D, 3, 3, 32));
        assert(has_insn(LISA_ST_D, 1, 3, 8));
        assert(has_insn(LISA_LD_D, 1, 3, 8));
        assert(has_insn(LISA_ST_D, 4, 3, 0));
        bool operand = kind == AOT_HELPER_X87 || kind == AOT_HELPER_SYSTEM;
        if (operand) {
            assert(has_insn(LISA_ST_D, 5, 3, 16));
            assert(has_insn(LISA_LDPTR_D, 5, 3, 4)); /* 16 >> 2 */
            assert(has_insn(LISA_LDPTR_D, 6, 3, 0));
        } else {
            assert(has_insn(LISA_LDPTR_D, 5, 3, 0));
        }

        int jirl = find_insn(0, LISA_JIRL, 1, 1, 0);
        assert(jirl > 0);
#ifdef CONFIG_LATX_OPT_NOFP
        /* Before JIRL: save env->nofp to sp+24, store 8 MMX FPRs, clear env->nofp. */
        int save_nofp = find_insn(0, LISA_ST_W, 4, 3, 24);
        int clear_nofp = find_insn(0, LISA_ST_W, 0, 23, nofp_off);
        assert(save_nofp >= 0 && save_nofp < jirl);
        assert(clear_nofp > save_nofp && clear_nofp < jirl);
        assert(count_op_in_range(0, (unsigned)jirl, LISA_FST_D) == 8);

        /* After JIRL: reload sp+24, reload 8 MMX FPRs, restore env->nofp. */
        int reload_nofp = find_insn((unsigned)jirl + 1, LISA_LD_W, 4, 3, 24);
        int restore_nofp = find_insn((unsigned)jirl + 1, LISA_ST_W, 4, 23, nofp_off);
        assert(reload_nofp > jirl);
        assert(restore_nofp > reload_nofp);
        assert(count_op_in_range((unsigned)jirl + 1, emitted_count, LISA_FLD_D) == 8);
#else
        assert(count_op_in_range(0, (unsigned)jirl, LISA_FST_D) == 8);
        assert(count_op_in_range((unsigned)jirl + 1, emitted_count, LISA_FLD_D) == 8);
#endif
    }

    /* 4. Epilogue jump_cold: fixed entry offsets intact, 48-byte private frame (non-overlapping
     * with sp+24 replay slot), and symmetrical FPR / NOFP MMX save + restore across C lookup. */
    gen_latxs_sc_epilogue(dummy_code);
    assert(emitted[0].op == LISA_OR && emitted[1].op == LISA_B);
    assert(emitted[2].op == LISA_ORI && emitted[2].imm == 3 && emitted[3].op == LISA_B);
    assert(emitted[4].op == LISA_ORI && emitted[4].imm == 1 && emitted[5].op == LISA_B);
    assert(emitted[6].op == LISA_ORI && emitted[6].imm == 0);
    {
        int epi_ret = find_insn(0, LISA_JIRL, 0, 1, 0);
        assert(epi_ret > 6);
        unsigned jc_start = (unsigned)epi_ret + 1;
        assert(jc_start < emitted_count);

        assert(find_insn_in_range(jc_start, emitted_count, LISA_ADDI_D, 3, 3, -32) == -1);
        assert(find_insn_in_range(jc_start, emitted_count, LISA_ADDI_D, 3, 3, 32) == -1);
        /* jump_cold must not use the one-way latxs_gen_nofp_sync_fprs_to_env helper. */
        assert(find_insn_in_range(jc_start, emitted_count, 902, -2, -2, -999999) == -1);

        int alloc_sp = find_insn_in_range(jc_start, emitted_count, LISA_ADDI_D, 3, 3, -48);
        int jump_jirl = find_insn_in_range(jc_start, emitted_count, LISA_JIRL, 1, 1, 0);
        int save_replay = find_insn_in_range((unsigned)jump_jirl + 1, emitted_count,
                                             LISA_STPTR_D, 4, 3, 6);
        int replay_jirl = find_insn_in_range((unsigned)jump_jirl + 1, emitted_count,
                                             LISA_JIRL, 0, 4, 0);
        assert(alloc_sp >= (int)jc_start && alloc_sp < jump_jirl);
        assert(save_replay > jump_jirl && replay_jirl > save_replay);

        assert(count_op_in_range(jc_start, (unsigned)jump_jirl, LISA_FST_D) == 8);
        assert(count_op_in_range((unsigned)jump_jirl + 1, (unsigned)replay_jirl, LISA_FLD_D) == 8);
#ifdef CONFIG_LATX_OPT_NOFP
        int jc_load_nofp = find_insn_in_range(jc_start, (unsigned)jump_jirl,
                                              LISA_LD_W, 4, 23, nofp_off);
        int jc_save_nofp = find_insn_in_range(jc_start, (unsigned)jump_jirl,
                                              LISA_ST_W, 4, 3, 32);
        int jc_clear_nofp = find_insn_in_range(jc_start, (unsigned)jump_jirl,
                                               LISA_ST_W, 0, 23, nofp_off);
        assert(jc_load_nofp > alloc_sp);
        assert(jc_save_nofp > jc_load_nofp && jc_clear_nofp > jc_save_nofp);
        assert(jc_clear_nofp < jump_jirl);

        int jc_reload_nofp = find_insn_in_range((unsigned)save_replay + 1, (unsigned)replay_jirl,
                                                LISA_LD_W, 4, 3, 32);
        int jc_restore_nofp = find_insn_in_range((unsigned)save_replay + 1, (unsigned)replay_jirl,
                                                 LISA_ST_W, 4, 23, nofp_off);
        int reload_replay = find_insn_in_range((unsigned)save_replay + 1, (unsigned)replay_jirl,
                                               LISA_LDPTR_D, 4, 3, 6);
        assert(jc_reload_nofp > save_replay);
        assert(jc_restore_nofp > jc_reload_nofp);
        assert(reload_replay > jc_restore_nofp && reload_replay < replay_jirl);
#endif
        int free_sp_ok = find_insn_in_range((unsigned)save_replay + 1, (unsigned)replay_jirl,
                                            LISA_ADDI_D, 3, 3, 48);
        int free_sp_fail = find_insn_in_range((unsigned)replay_jirl + 1, emitted_count,
                                              LISA_ADDI_D, 3, 3, 48);
        assert(free_sp_ok > save_replay && free_sp_ok < replay_jirl);
        assert(free_sp_fail > replay_jirl);
    }

    puts("PASS: inline helper prologues/epilogues, AOT stubs, and jump_cold preserve live $fcsr0 and FPR/NOFP MMX state");
    return 0;
}
"""
)

with tempfile.TemporaryDirectory(prefix="lats-inline-fcsr-") as folder:
    binary = Path(folder) / "check"
    for extra in ([], ["-DCONFIG_LATX_OPT_NOFP"], ["-DCONFIG_LATX_OPT_EFLAGS"],
                  ["-DCONFIG_LATX_OPT_NOFP", "-DCONFIG_LATX_OPT_EFLAGS"]):
        subprocess.run(
            ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", *extra,
             "-x", "c", "-o", str(binary), "-"],
            input=program,
            text=True,
            check=True,
        )
        subprocess.run([str(binary)], check=True)
