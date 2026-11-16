#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Compile the HRAS option and return-exploration guards from production."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
options = (root / 'target/i386/latx/latx-options.c').read_text()
header = (root / 'target/i386/latx/include/latx-options.h').read_text()
tu = (root / 'target/i386/latx/optimization/sys-tu.c').read_text()
eob = (root / 'target/i386/latx/translator/sys-eob.c').read_text()
translate = (root / 'target/i386/latx/translator/translate.c').read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
static int option_tu_hras, option_cross_page_tu, option_tucp_fast;
static int option_intb_njc, warnings;
static int jit_requested = 1;
static bool jit_tu_requested(void) { return jit_requested; }
#define error_report(...) (++warnings)
#define warn_report(...) (++warnings)
'''
for name in ('ENABLE', 'DIRECT_CHECK', 'JMP_CACHE', 'FSHT_CHECK', 'LOOKUP_CHECK'):
    c += re.search(r'^#define TUCPFAST_' + name + r'\s+[^\n]+', header, re.M)[0] + '\n'
for signature in ('static inline int cp_tu(void)',
                  'static inline int cp_tu_pages(void)'):
    start = header.index(signature)
    c += header[start:header.index('\n', start)] + '\n'
for name in ('tucp_fast', 'tucp_fast_direct_check', 'tucp_fast_needs_pages_desc',
             'tucp_fast_fsht_check', 'tucp_fast_lookup_check'):
    c += function(header, 'static inline bool ' + name + '(') + '\n'
c += function(options, 'static void latxs_options_check_tu_hras(') + '\n'
assert options.index('latxs_options_check_tu_hras();') > options.index(
    'if (option_tucp_fast & ~TUCPFAST_ALL)')

# Compile the actual CALL return exploration, including retry/error exits.
c += r'''
typedef uint64_t ADDRX;
typedef uint64_t target_ulong;
typedef struct TranslationBlock TranslationBlock;
enum { IR1_TYPE_RET, IR1_TYPE_CALL, IR1_TYPE_CALLIN };
struct data {
    TranslationBlock *next_tb[2];
    ADDRX x86_next_pc;
    int last_ir1_type;
};
struct TranslationBlock {
    ADDRX pc, cs_base; unsigned flags, icount; struct data *s_data;
    struct { void *ptr; } tc;
    unsigned inst_ptr_offset, bool_flags;
    uint16_t jmp_insn_offset[2], jmp_reset_offset[2];
    uint32_t jmp_cp_data[2];
    uint8_t link_opt[2];
    uint64_t jmp_inst_hold[2];
};
typedef struct { int unused; } CPUState;
#define TU_TB_INDEX_NEXT 0
#define MAX_TB_IN_TS 16
#define TU_TB_START_NORMAL 0
#define TU_EXPLORE_OK 0
#define get_page(pc) ((pc) & ~4095ULL)
static struct {
    uint32_t tb_num;
    TranslationBlock *tb_list[MAX_TB_IN_TS];
} storage, *tu_data = &storage;
static int cross_enabled, mapped, creates, pushes, fail_create;
static bool tu_cross_page_enabled(void) { return cross_enabled; }
static bool page_valid(CPUState *cpu, ADDRX pc)
{ (void)cpu; (void)pc; return mapped; }
static TranslationBlock *tu_tree_lookup(ADDRX pc) { (void)pc; return NULL; }
static int tu_pages_find(ADDRX pc) { (void)pc; return 0; }
static bool tu_pages_append(ADDRX pc) { (void)pc; return true; }
static TranslationBlock *tb_htable_lookup_noexit(CPUState *cpu, ADDRX pc,
    target_ulong cs, uint32_t flags, int cflags)
{ (void)cpu; (void)pc; (void)cs; (void)flags; (void)cflags; return NULL; }
static struct data created_data;
static TranslationBlock created = { .s_data = &created_data };
static TranslationBlock *tb_create(CPUState *cpu, ADDRX pc, int base,
    int flags, int cflags, int max, int zero, int type,
    TranslationBlock *caller, int *result)
{
    (void)cpu; (void)pc; (void)base; (void)flags; (void)cflags;
    (void)max; (void)zero; (void)type;
    assert(caller); creates++; *result = fail_create; return &created;
}
static void tu_push_back(TranslationBlock *tb)
{ assert(tb); pushes++; tu_data->tb_list[tu_data->tb_num++] = tb; }
static void tu_pages_tb_add(TranslationBlock *tb) { assert(tb); }
'''
c += function(tu, 'static TranslationBlock *tu_hras_original_member(') + '\n'
c += function(tu, 'static int tu_hras_complete_returns(')
c += r'''
static int explore(ADDRX next_pc)
{
    struct data data = { .last_ir1_type = IR1_TYPE_CALL,
                         .x86_next_pc = next_pc };
    TranslationBlock call = { .pc = 4091, .s_data = &data };
    uint32_t scanned = 0;

    storage.tb_num = 1;
    storage.tb_list[0] = &call;
    return tu_hras_complete_returns(NULL, 0, 0, 0, 0, &scanned, NULL);
}
/* Compile the actual indirect CALL hint condition, including any CPU gate. */
#define HF_CS64_MASK (1u << 15)
#define HF_CPL_MASK 3u
static int option_rastoptp, option_soft_ras, option_aot_save, in_aot_translate;
static bool latxs_hras_jit_translate(const TranslationBlock *tb)
{ (void)tb; return !in_aot_translate && !option_aot_save; }
static bool latxs_hras_aot_translate(void) { return false; }
static int mttcg;
static struct { struct { int max_cpus; } smp; } machine;
#define current_machine (&machine)
bool qemu_tcg_mttcg_enabled(void) { return mttcg; }
static bool ir1_is_indirect_call(int branch) { (void)branch; return true; }
static int latxs_ir1_data_size(int branch) { (void)branch; return 64; }
ADDRX ir1_addr_next(int branch) { (void)branch; return 4096; }
bool tb_is_adjacent(TranslationBlock *tb, TranslationBlock *next)
{ (void)tb; (void)next; return true; }
static bool indirect_hint(bool inline_tu_exit, bool has_return, bool pushed,
                          bool *dedicated_miss)
{
    struct data data = {0};
    TranslationBlock call = { .flags = HF_CS64_MASK | HF_CPL_MASK,
                              .s_data = &data };
    TranslationBlock ret = { .pc = 4096, .icount = 1 };
    struct { bool ras_tp_has_call; } tr = { pushed };
    struct { TranslationBlock *curr_tb; typeof(tr) *tr_ctx; } state = { &call, &tr };
    typeof(state) *ctx = &state;
    TranslationBlock *next = has_return ? &ret : NULL;
    bool hras_call, inline_hras_call;
    int branch = 0;
    (void)next; (void)inline_tu_exit;
'''
start = eob.index('    hras_call = option_tu_hras &&')
c += eob[start:eob.index(';', start) + 1]
start = eob.index('    inline_hras_call = hras_call &&')
c += '\n' + eob[start:eob.index(';', start) + 1]
c += r'''
    *dedicated_miss = inline_hras_call;
    return hras_call;
}
/* Compile the actual linker: nonadjacent/ordinary CALLs use generic slots. */
#define CONFIG_LATX_TU 1
#define CONFIG_SOFTMMU 1
#define IS_HRAS_CALL 0x80
#define IS_AOT_TB 0x08
#define TB_JMP_CP_DATA_VALID 0x80000000u
#define TB_JMP_CP_DATA_LINKED 0x40000000u
#define TB_JMP_CP_DATA_OFF 0xffffu
#define g_assert assert
#define qatomic_read(p) (*(p))
#define qatomic_set(p, value) (*(p) = (value))
static int64_t sextract64(uint64_t v, unsigned start, unsigned length)
{ return (int64_t)(v << (64 - start - length)) >> (64 - length); }
static void flush_idcache_range(uintptr_t a, uintptr_t b, unsigned n)
{ (void)a; (void)b; (void)n; }
'''
c += function(translate, 'static void __latxs_tb_set_jmp_target(')
c += r'''
static void check_direct_link(bool far, int n, bool push, bool aot,
                              unsigned flags, ADDRX base, bool expected)
{
    uint64_t code[1] = {UINT64_C(0x03400000) << 32};
    TranslationBlock tb = { .flags = flags, .cs_base = base,
        .tc.ptr = code, .inst_ptr_offset = push ? 4 : 0,
        .bool_flags = aot ? IS_AOT_TB : 0 };
    uintptr_t target = (uintptr_t)code + (far ? 0x20000000 : 64);
    __latxs_tb_set_jmp_target(&tb, n, (void *)target, false);
    uint32_t *words = (uint32_t *)code;
    if (far) {
        assert((words[0] & 0xfe000000) == 0x1e000000);
        assert((words[1] & 0xfc000000) == 0x4c000000);
        assert((words[1] & 31) == (expected ? 1u : 0u));
    } else {
        assert((words[0] & 0xfc000000) ==
               (expected ? 0x54000000 : 0x50000000));
        assert(words[1] == 0x03400000); /* Short links retain the reserved NOP. */
    }
}

static int64_t branch_displacement(uint32_t inst)
{
    int64_t imm = ((inst >> 10) & 0xffff) |
                  ((int64_t)(inst & 0x3ff) << 16);
    return sextract64(imm, 0, 26);
}

static void check_hras_call_link(bool far, int mode)
{
    const uint64_t initial = (UINT64_C(0x03400000) << 32) |
                             UINT64_C(0x50000800);
    uint64_t code[1] = {initial};
    TranslationBlock tb = { .flags = 7,
        .tc.ptr = code, .inst_ptr_offset = 4,
        .bool_flags = IS_HRAS_CALL,
        .jmp_insn_offset = {0}, .jmp_reset_offset = {8},
        .link_opt = {1} };
    uintptr_t target = (uintptr_t)code + (far ? 0x20000000 : 64);
    uint32_t *words = (uint32_t *)code;

    option_tu_hras = mode;
    __latxs_tb_set_jmp_target(&tb, 0, (void *)target, false);
    assert(tb.link_opt[0] == 2);
    assert(tb.jmp_inst_hold[0] == initial);
    if (far) {
        assert((words[0] & 0xfe000000) == 0x1e000000);
        assert((words[1] & 0xfc000000) == 0x4c000000);
        assert((words[1] & 31) == (mode == 2 ? 1u : 0u));
    } else {
        assert(words[0] == 0x03400000);
        assert((words[1] & 0xfc000000) ==
               (mode == 2 ? 0x54000000 : 0x50000000));
        assert((uintptr_t)(words + 1) +
               (branch_displacement(words[1]) << 2) == target);
    }
}
int main(void)
{
'''
c += r'''
    check_hras_call_link(false, 1);
    check_hras_call_link(false, 2);
    check_hras_call_link(true, 1);
    check_hras_call_link(true, 2);

    /* Enumerate every raw check mask, with and without implicit NJC FSHT. */
    for (int mask = 0; mask < 64; mask++) {
        for (int njc = 0; njc < 2; njc++) {
            for (int mode = 0; mode <= 2; mode++) {
                option_tucp_fast = mask; option_intb_njc = njc;
                option_cross_page_tu = 9; option_tu_hras = mode;
                bool allowed = (mask & 1) && (mask & 4) && (mask & 32) &&
                               ((mask & 16) || njc);
                warnings = 0; latxs_options_check_tu_hras();
                assert(option_tu_hras == (allowed ? mode : 0));
                assert(warnings == (!allowed && mode != 0));
            }
        }
    }
    option_tucp_fast = 63;
    for (int cp = 0; cp < 16; cp++) {
        option_cross_page_tu = cp; option_tu_hras = 2;
        latxs_options_check_tu_hras();
        assert(option_tu_hras == ((cp & 9) == 9 ? 2 : 0));
    }
    option_cross_page_tu = 9;
    jit_requested = 0; option_tu_hras = 2; warnings = 0;
    latxs_options_check_tu_hras();
    assert(option_tu_hras == 0 && warnings == 1);
    jit_requested = 1;
    for (int cross = 0; cross < 2; cross++) {
        for (int valid = 0; valid < 2; valid++) {
            cross_enabled = cross; mapped = valid; creates = pushes = 0;
            assert(explore(4096) == 0);
            assert(creates == (cross && valid)); assert(pushes == creates);
            creates = pushes = 0;
            assert(explore(4095) == 0); /* Same-page returns survive retry. */
            assert(creates == 1 && pushes == 1);
        }
    }
    option_rastoptp = option_soft_ras = 1;
    for (mttcg = 0; mttcg <= 1; mttcg++) {
        for (machine.smp.max_cpus = 1; machine.smp.max_cpus <= 2;
             machine.smp.max_cpus++) {
            for (option_tu_hras = 0; option_tu_hras <= 2; option_tu_hras++) {
                for (int in_tu = 0; in_tu < 2; in_tu++) {
                    for (int has_return = 0; has_return < 2; has_return++) {
                        for (int push = 0; push < 2; push++) {
                            for (int njc_enabled = 0; njc_enabled < 2; njc_enabled++) {
                                bool dedicated;
                                bool hint = option_tu_hras != 0 && push;
                                option_intb_njc = njc_enabled;
                                assert(indirect_hint(in_tu, has_return, push,
                                                     &dedicated) == hint);
                                assert(dedicated == (hint && njc_enabled));
                            }
                        }
                    }
                }
            }
        }
    }
    unsigned user64 = HF_CS64_MASK | HF_CPL_MASK;
    for (option_tu_hras = 0; option_tu_hras <= 2; option_tu_hras++) {
        for (int far = 0; far < 2; far++) {
            bool hint = option_tu_hras == 2;
            check_direct_link(far, 0, true, false, user64, 0, hint);
            check_direct_link(far, 1, true, false, user64, 0, false);
            check_direct_link(far, 0, false, false, user64, 0, false);
            check_direct_link(far, 0, true, true, user64, 0, false);
            check_direct_link(far, 0, true, false, HF_CS64_MASK, 0, false);
            check_direct_link(far, 0, true, false, HF_CPL_MASK, 0, false);
            check_direct_link(far, 0, true, false, user64, 4096, false);
            option_aot_save = 1;
            check_direct_link(far, 0, true, false, user64, 0, false);
            option_aot_save = 0; in_aot_translate = 1;
            check_direct_link(far, 0, true, false, user64, 0, false);
            in_aot_translate = 0;
        }
    }
    cross_enabled = mapped = 1; fail_create = 7; pushes = 0;
    assert(explore(4096) == 7 && pushes == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tu-hras-guards-') as directory:
    source = Path(directory) / 'guards.c'
    binary = Path(directory) / 'guards'
    source.write_text(c)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', *shlex.split(os.environ.get('CFLAGS', '')),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: HRAS checks, retry, and ordinary/TU near/far CALL link pairing')

# Exercise the complete action, so an early return cannot skip an IRQ label.
exit_c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define CONFIG_LATX_TU 1
#define CONFIG_SOFTMMU 1
#define HF_CS64_MASK 128
#define HF_CPL_MASK 3
#define X86_INS_RET 10
#define IRQ_CHECK_BLOCK_END 2
#define IRQ_CHECK_ASYNC_NOTIFY 3
typedef int IR2_OPND;
typedef struct { bool valid; IR2_OPND miss, call, target;
    bool fsht_deferred; IR2_OPND fsht_scratch[5]; } HrasIndirectCallLabels;
typedef struct { unsigned flags; int cs_base; } TranslationBlock;
typedef struct { bool opt_can_link, opt_can_indir_link, ras_tp_has_call;
    TranslationBlock *curr_tb; HrasIndirectCallLabels hras_indirect_call; } Data;
typedef struct { TranslationBlock *curr_tb; Data *tr_ctx; int branch_opcode; } ExitTBContext;
typedef struct { bool saved_next_eip; } ExitTBEmitState;
typedef struct { bool call; } IR1_INST;
static int option_int_opt, option_tu_hras, option_rastoptp = 1, option_soft_ras = 1;
static int in_aot_translate, option_aot_save;
static bool latxs_hras_jit_translate(const TranslationBlock *tb)
{ (void)tb; return !in_aot_translate && !option_aot_save; }
static bool latxs_hras_aot_translate(void) { return false; }
'''
exit_c += function(
    header, 'static inline bool latxs_check_interrupt_at_block_end(')
exit_c += r'''
static bool use_tu_exit, referenced, inserted;
static bool option_intb_njc, option_flaged_sht, fsht_valid;
static int njc_calls, fsht_calls, generic_calls, context_exits, prefix_calls;
static int la_new_label(void) { static int n; return ++n; }
static bool ir1_is_indirect_call(IR1_INST *branch) { return branch->call; }
static int latxs_ir1_data_size(IR1_INST *branch) { (void)branch; return 64; }
static bool latx_jmp_cache_compact_enabled(void) { return true; }
static int fsht_flags_to_id(unsigned flags) { (void)flags; return fsht_valid ? 1 : -1; }
static bool tucp_fast_fsht_check(void) { return true; }
static void gen_latxs_intb_fsht_call(int fid, bool fast,
    IR2_OPND *miss, IR2_OPND *call, IR2_OPND *target)
{ assert(fid == 1 && fast); (void)miss; (void)call; (void)target; fsht_calls++; }
static void gen_latxs_intb_fsht_call_njc(int fid,
    IR2_OPND *miss, IR2_OPND *call, IR2_OPND *target)
{ assert(fid == 1); (void)miss; (void)call; (void)target; fsht_calls++; }
static void gen_latxs_intb_fsht_call_njc_save(int fid,
    IR2_OPND *miss, IR2_OPND *call, IR2_OPND *target, void *scratch)
{ (void)scratch; gen_latxs_intb_fsht_call_njc(fid, miss, call, target); }
static void tr_gen_branch_save_next_eip(IR1_INST *branch, int id)
{ (void)branch; (void)id; }
static bool latxs_tr_exit_tb_use_tu_indirect_inline_exit(
    const ExitTBContext *ctx, IR1_INST *branch, int id)
{ (void)ctx; (void)branch; (void)id;
  return use_tu_exit && latxs_check_interrupt_at_block_end(); }
static void latxs_tr_exit_tb_emit_indirect_context_switch(const ExitTBContext *ctx, int id)
{ (void)ctx; (void)id; context_exits++; }
/* Model the label reference emitted by the ordinary block-end IRQ check. */
static void latxs_tr_exit_tb_emit_indirect_interrupt(const ExitTBContext *ctx,
    int id, bool inline_tu, IR2_OPND *miss, int fsht_id)
{ (void)id;
  if (inline_tu && fsht_id >= 0) { assert(miss); prefix_calls++; }
  referenced = latxs_check_interrupt_at_block_end() &&
      ctx->branch_opcode != X86_INS_RET && !inline_tu; }
static void latxs_ir2_interrupt_set_label(TranslationBlock *tb, int id)
{ (void)tb; (void)id; inserted = true; }
static void gen_latxs_intb_lookup_njc_call(bool compact,
    IR2_OPND *miss, IR2_OPND *call, IR2_OPND *target)
{ (void)compact; (void)miss; (void)call; (void)target; njc_calls++; }
static void latxs_tr_exit_tb_emit_indirect_lookup(const ExitTBContext *ctx, bool hras)
{ (void)ctx; (void)hras; generic_calls++; }
'''
exit_c += function(eob, 'static void latxs_tr_exit_tb_emit_indirect_slowpath(')
exit_c += function(eob, 'static void latxs_tr_exit_tb_emit_indirect_action(')
exit_c += r'''
int main(void)
{
    TranslationBlock tb = { HF_CS64_MASK | HF_CPL_MASK, 0 };
    Data data = { .opt_can_link = true, .opt_can_indir_link = true,
                  .ras_tp_has_call = true, .curr_tb = &tb };
    ExitTBContext ctx = { &tb, &data, 0 };
    ExitTBEmitState state = { false };
    IR1_INST branch = { true };
    for (int irq = 0; irq <= IRQ_CHECK_ASYNC_NOTIFY; irq++)
    for (int hras = 0; hras <= 2; hras++)
    for (int tu_exit = 0; tu_exit <= 1; tu_exit++)
    for (int njc_enabled = 0; njc_enabled <= 1; njc_enabled++)
    for (int fsht = 0; fsht <= 1; fsht++)
    for (int valid = 0; valid <= 1; valid++) {
        option_flaged_sht = fsht; fsht_valid = valid;
        option_int_opt = irq; option_tu_hras = hras;
        bool checked = irq == IRQ_CHECK_BLOCK_END ||
                       irq == IRQ_CHECK_ASYNC_NOTIFY;
        use_tu_exit = tu_exit; option_intb_njc = njc_enabled;
        referenced = inserted = false;
        njc_calls = fsht_calls = generic_calls = context_exits = prefix_calls = 0;
        latxs_tr_exit_tb_emit_indirect_action(&ctx, &state, &branch, 0);
        assert(!referenced || inserted);
        assert(prefix_calls == !!(hras && njc_enabled && fsht && valid &&
                                  tu_exit && checked));
        assert(fsht_calls == !!(hras && njc_enabled && fsht && valid));
        assert(njc_calls == !!(hras && njc_enabled && !(fsht && valid)));
        assert(generic_calls == !(njc_calls || fsht_calls));
        assert(context_exits == (checked && !tu_exit));
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tu-hras-exit-') as directory:
    source = Path(directory) / 'exit.c'
    binary = Path(directory) / 'exit'
    source.write_text(exit_c)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', *shlex.split(os.environ.get('CFLAGS', '')),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: ordinary inline CALLs resolve their interrupt exit labels')

# A TU can exceed a conditional branch's reach before its size is assembled.
range_c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
typedef int IR2_OPCODE;
enum { LISA_LABEL, LISA_X86_INST, LISA_TU_SPLIT, LISA_PROFILE,
       LISA_BEQ, LISA_BNE, LISA_ADD_D, LISA_BLT, LISA_BGE };
static unsigned option_irq_outline;
typedef struct { int id; bool is_label; } IR2_OPND;
typedef struct { IR2_OPCODE opcode; IR2_OPND target; } IR2_INST;
typedef struct { int ir2_cur_nr, label_num;
    bool irq_backedge_short, hras_long_checks; } TRANSLATION_DATA;
static TRANSLATION_DATA data;
static struct { TRANSLATION_DATA *tr_data; } env = { &data }, *lsenv = &env;
static IR2_INST instructions[32770];
#define g_assert assert
static IR2_INST *ir2_get(int i) { return &instructions[i]; }
static IR2_OPCODE ir2_opcode(IR2_INST *inst) { return inst->opcode; }
static IR2_OPND *latxs_ir2_branch_get_label(IR2_INST *inst) { return &inst->target; }
static bool latxs_ir2_opnd_is_label(IR2_OPND *label) { return label->is_label; }
static int latxs_ir2_opnd_label_id(IR2_OPND *label) { return label->id; }
'''
range_c += function(tu, 'static bool tu_hras_njc_branches_fit(')
range_c += r'''
#define TU_ASSEMBLE_RETRY_HRAS_LONG_CHECK -3
#define TU_ASSEMBLE_ERR_HRAS_NJC_RANGE -4
static bool direct_fit, inline_enabled;
static int option_tu_hras, direct_checks;
static bool intb_njc_inline_enabled(void) { return inline_enabled; }
static bool latxs_hras_direct_checks_fit(uint32_t count, void *const *calls,
                                         const int *positions)
{ (void)count; (void)positions; assert(calls); direct_checks++; return direct_fit; }
static int preflight(int target, bool has_inline_hras_call)
{
    TRANSLATION_DATA *td = &data;
    int *label_pos = malloc(2 * sizeof(int));
    int *label_pos_id = calloc(2, sizeof(int));
    assert(label_pos && label_pos_id);
    label_pos[0] = -1; label_pos[1] = target;
    int tb_num_in_tu = 0;
    void *calls[1] = { NULL };
    void *const *direct_calls = option_tu_hras ? calls : NULL;
    bool use_malloc = true;
'''
start = tu.index('    /* No TB offset has been converted from a label yet.')
range_c += tu[start:tu.index('    fix_tu_cold_exit_offsets(', start)]
range_c += '    free(label_pos); free(label_pos_id);\n    return 0;\n}\n'
range_c += r'''
int main(void)
{
    int positions[2] = { -1, 0 };
    data.ir2_cur_nr = 6; data.label_num = 1;
    instructions[0].opcode = LISA_LABEL;
    instructions[1].opcode = LISA_X86_INST;
    instructions[2].opcode = LISA_TU_SPLIT;
    instructions[3].opcode = LISA_PROFILE;
    instructions[4].opcode = LISA_ADD_D;
    instructions[5].target = (IR2_OPND){ 1, true };
    option_irq_outline = 3;
    int checked_opcodes[] = {LISA_BEQ, LISA_BNE, LISA_BLT, LISA_BGE};
    for (unsigned which = 0; which < 4; which++) {
        int opcode = checked_opcodes[which];
        instructions[5].opcode = opcode;
        positions[1] = 1 + 32767;
        assert(tu_hras_njc_branches_fit(positions));
        positions[1]++;
        assert(!tu_hras_njc_branches_fit(positions));
        /* The first member of a 1024-CALL inline TU can reach this far. */
        positions[1] = 60000;
        assert(!tu_hras_njc_branches_fit(positions));
        /* An uninserted label retains the existing TU handling. */
        positions[1] = -1;
        assert(tu_hras_njc_branches_fit(positions));
    }
    for (int i = 0; i < 32769; i++) instructions[i].opcode = LISA_ADD_D;
    data.ir2_cur_nr = 32770;
    instructions[32769] = (IR2_INST){ LISA_BNE, { 1, true } };
    positions[1] = 0;
    assert(!tu_hras_njc_branches_fit(positions));
    positions[1] = 1;
    assert(tu_hras_njc_branches_fit(positions));
    /* A direct CALL must get its existing long-form retry first. */
    data.ir2_cur_nr = 1;
    instructions[0] = (IR2_INST){ LISA_BNE, { 1, true } };
    option_tu_hras = 2; inline_enabled = true; direct_fit = false;
    assert(preflight(32768, false) == TU_ASSEMBLE_RETRY_HRAS_LONG_CHECK);
    direct_fit = true;
    assert(preflight(32768, false) == TU_ASSEMBLE_ERR_HRAS_NJC_RANGE);
    assert(preflight(32767, false) == 0);
    /* CALL-only inline is checked even with the global inline flag off. */
    inline_enabled = false;
    assert(preflight(32768, true) == TU_ASSEMBLE_ERR_HRAS_NJC_RANGE);
    assert(preflight(32767, true) == 0);
    /* A TU without an inline CALL retains the old scan condition. */
    assert(preflight(32768, false) == 0);
    option_tu_hras = 0;
    direct_fit = false;
    int checked = direct_checks;
    assert(preflight(32768, false) == 0);
    assert(direct_checks == checked); /* Disabled HRAS skips its empty check. */
    /* A direct-backedge-only TU has no inline CALL to trigger the old gate.
     * Its short IRQ edge must still be checked before metadata mutation. */
    option_tu_hras = 2; option_irq_outline = 0; direct_fit = true;
    inline_enabled = false; data.irq_backedge_short = true;
    data.hras_long_checks = false;
    instructions[0] = (IR2_INST){ LISA_BLT, { 1, true } };
    assert(preflight(32768, false) == TU_ASSEMBLE_RETRY_HRAS_LONG_CHECK);
    assert(preflight(32767, false) == 0);
    for (int i = 0; i < 32769; i++) instructions[i].opcode = LISA_ADD_D;
    data.ir2_cur_nr = 32770;
    instructions[32769] = (IR2_INST){ LISA_BLT, { 1, true } };
    assert(preflight(0, false) == TU_ASSEMBLE_RETRY_HRAS_LONG_CHECK);
    assert(preflight(1, false) == 0);
    /* Retry emits the original local BGE over a far B. */
    data.ir2_cur_nr = 1; data.irq_backedge_short = false;
    data.hras_long_checks = true;
    instructions[0] = (IR2_INST){ LISA_BGE, { 1, true } };
    assert(preflight(2, false) == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tu-hras-njc-range-') as directory:
    source = Path(directory) / 'range.c'
    binary = Path(directory) / 'range'
    source.write_text(range_c)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', *shlex.split(os.environ.get('CFLAGS', '')),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: inline NJC conditional reach is checked before TU assembly')

# Compile the actual local-edge emitter and its pre-publication range fallback.
local_c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define HF_CS64_MASK 16
#define HF_CPL_MASK 3
#define TU_TB_INDEX_TARGET 1
#define TB_JMP_RESET_OFFSET_INVALID 65535
static int option_tu_hras = 2, option_rastoptp = 1, option_soft_ras = 1;
static int in_aot_translate, option_aot_save;
struct Data { uintptr_t tu_jmp[2]; uint32_t tu_link_ins; };
typedef struct {
    struct { void *ptr; uintptr_t offset_in_tu; } tc; struct Data *s_data;
    unsigned flags; uintptr_t cs_base;
    unsigned jmp_insn_offset[2], jmp_reset_offset[2], link_opt[2];
} TranslationBlock;
typedef struct { TranslationBlock *curr_tb; bool target_in_current_tu; } ExitTBContext;
typedef struct { bool emitted_interrupt_check; } ExitTBEmitState;
typedef struct { bool conditional; } IR1_INST;
static bool ir1_is_branch(IR1_INST *branch) { return branch->conditional; }
static unsigned cursor;
static uintptr_t get_current_inst_addr(TranslationBlock *tb)
{ return (uintptr_t)tb->tc.ptr + cursor * 4; }
enum { LISA_B };
static unsigned count;
static void latxs_append_ir2_opnda(int opcode, int value)
{ assert(opcode == LISA_B && value == 0); count++; cursor++; }
'''
local_c += function(eob, 'static bool tu_hras_emit_local_irq_branch(')
local_c += function(tu, 'static bool tu_hras_local_branches_fit(')
# The saved instruction must use the same TU-relative address as relocation.
start = tu.index('            tb->s_data->tu_link_ins =')
local_c += '\nstatic void save_local_instruction(TranslationBlock *tb)\n{\n'
local_c += tu[start:tu.index(';', start) + 1] + '\n}\n'
assert tu.index('static bool tu_hras_local_branches_fit(') < tu.index(
    '#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)\nstatic bool tu_hras_member_size_fits')
local_c += r'''
#define TU_TRANSLATE_FALLBACK 7
static unsigned finished;
static void latxs_tr_fini(void) { finished++; }
static int check_span(int code_nr)
{
'''
start = tu.index('    if (code_nr >= 0 && !tu_hras_local_branches_fit(code_nr))')
local_c += tu[start:tu.index('\n#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)', start)]
local_c += r'''
    return 0;
}
int main(void)
{
    struct Data data = { .tu_jmp = { 65535, 65535 } };
    TranslationBlock tb = { .tc.ptr = (void *)0x100000, .s_data = &data,
        .flags = HF_CS64_MASK | HF_CPL_MASK,
        .jmp_insn_offset = { 65535, 65535 },
        .jmp_reset_offset = { 65535, 65535 } };
    ExitTBContext ctx = { &tb, true };
    ExitTBEmitState state = { true };
    IR1_INST branch = { true };
    /* No cacheline phase adds a NOP or another branch to this local edge. */
    for (unsigned phase = 0; phase < 128; phase += 4) {
        cursor = phase / 4; count = 0;
        assert(tu_hras_emit_local_irq_branch(&ctx, &state, &branch, 1));
        assert(count == 1 && cursor == phase / 4 + 1);
        assert(data.tu_jmp[1] == phase);
        assert(tb.jmp_insn_offset[1] == 65535 && tb.jmp_reset_offset[1] == 65535);
        assert(tb.link_opt[1] == 0);
    }
    for (int disabled = 0; disabled < 12; disabled++) {
        int id = 1;
        option_tu_hras = 2; option_rastoptp = option_soft_ras = 1;
        in_aot_translate = option_aot_save = 0;
        ctx.target_in_current_tu = state.emitted_interrupt_check = true;
        branch.conditional = true; tb.cs_base = 0;
        tb.flags = HF_CS64_MASK | HF_CPL_MASK;
        switch (disabled) {
        case 0: option_tu_hras = 0; break;
        case 1: option_rastoptp = 0; break;
        case 2: option_soft_ras = 0; break;
        case 3: in_aot_translate = 1; break;
        case 4: option_aot_save = 1; break;
        case 5: id = 0; break;
        case 6: ctx.target_in_current_tu = false; break;
        case 7: state.emitted_interrupt_check = false; break;
        case 8: branch.conditional = false; break;
        case 9: tb.flags &= ~HF_CS64_MASK; break;
        case 10: tb.flags &= ~HF_CPL_MASK; break;
        case 11: tb.cs_base = 1; break;
        }
        cursor = 14; count = 0; data.tu_jmp[1] = 65535;
        assert(!tu_hras_emit_local_irq_branch(&ctx, &state, &branch, id));
        assert(count == 0 && cursor == 14 && data.tu_jmp[1] == 65535);
    }
    /* Every pair of words in a TU of at most 2^25 words fits signed B26. */
    for (int mode = 1; mode <= 2; mode++) {
        option_tu_hras = mode; finished = 0;
        assert(check_span((1 << 25) - 1) == 0);
        assert(check_span(1 << 25) == 0);
        assert(check_span((1 << 25) + 1) == TU_TRANSLATE_FALLBACK);
        assert(finished == 1);
        assert(check_span(-1) == 0); /* keep existing assembly error handling */
    }
    option_tu_hras = 0;
    assert(check_span(INT32_MAX) == 0);
    uint32_t code[32];
    for (unsigned i = 0; i < 32; i++) code[i] = i;
    tb.tc.ptr = &code[10]; tb.tc.offset_in_tu = 40;
    data.tu_jmp[1] = 56;
    save_local_instruction(&tb);
    assert(data.tu_link_ins == code[14]);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tu-hras-local-irq-') as directory:
    source = Path(directory) / 'local.c'
    binary = Path(directory) / 'local'
    source.write_text(local_c)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', *shlex.split(os.environ.get('CFLAGS', '')),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: fixed TU IRQ edges need one word and reject an out-of-range TU')

# Runtime linking patches a pre-reserved gate instead of rebuilding the TU.
ras = (root / 'target/i386/latx/optimization/sys-ras.c').read_text()
link_c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define HF_CS64_MASK 1
#define HF_CPL_MASK 6
#define IS_AOT_TB 1
#define IS_HRAS_CALL 128
#define CF_INVALID 0x40000
#define IR1_TYPE_CALL 5
#define TB_JMP_CP_DATA_VALID 0x100
#define TB_JMP_CP_DATA_OFF 0xff
typedef struct CPUState { int unused; } CPUState;
typedef struct TranslationBlock TranslationBlock;
struct separated_data { int last_ir1_type; TranslationBlock *next_tb[2];
    bool has_hras_check_gate; };
struct TranslationBlock {
    uint32_t flags, cflags, bool_flags, inst_ptr_offset, jmp_cp_data[2];
    unsigned jmp_insn_offset[2], jmp_reset_offset[2];
    uintptr_t pc, size, cs_base;
    unsigned icount;
    struct { void *ptr; } tc;
    struct separated_data *s_data;
    bool is_tu;
    void *pages;
    unsigned identity;
};
static int option_tu_hras, option_rastoptp, option_soft_ras, option_aot_save;
static bool checked_valid, unchecked_valid;
static unsigned opened, closed, writes;
static bool open_ok = true;
#define tb_cflags(t) ((t)->cflags)
#define tb_is_tu(t) ((t)->is_tu)
#define TB_TU_PAGES(t) ((t)->pages)
#define tb_tu_identity(t) ((t)->identity)
static bool latxs_hras_call_target_can_link(TranslationBlock *s,
        TranslationBlock *t, uintptr_t site, bool checked)
{
    (void)s; (void)t; (void)site;
    return checked ? checked_valid : unchecked_valid;
}
static void qemu_thread_jit_write(void) { writes++; }
static bool latxs_hras_open_check_gate(TranslationBlock *tb, uintptr_t hot)
{
    (void)tb; (void)hot; opened++; return open_ok;
}
static void latxs_hras_close_check_gate(TranslationBlock *tb)
{
    (void)tb; closed++;
}
'''
link_c += function(ras, 'static uint32_t *hras_check_gate(') + '\n'
link_c += function(ras, 'bool latxs_hras_prepare_link(')
link_c += r'''
static void reset_counts(void) { opened = closed = writes = 0; }
int main(void)
{
    CPUState cpu = {0};
    struct separated_data data = {.last_ir1_type = IR1_TYPE_CALL,
        .has_hras_check_gate = true};
    TranslationBlock next = {.pc = 105, .icount = 1};
    TranslationBlock target = {.pc = 200};
    TranslationBlock source = {.flags = 7, .bool_flags = IS_HRAS_CALL,
        .inst_ptr_offset = 24, .jmp_cp_data = {0x105, 0},
        .jmp_insn_offset = {64, 0}, .jmp_reset_offset = {72, 0},
        .pc = 100, .size = 5, .tc = {(void *)0x1000}, .s_data = &data,
        .is_tu = true};
    data.next_tb[1] = &next;
    option_tu_hras = 2; option_rastoptp = option_soft_ras = 1;
    option_aot_save = 0;

    reset_counts(); checked_valid = unchecked_valid = true;
    assert(latxs_hras_prepare_link(&cpu, &source, 0, &target));
    assert(opened == 1 && closed == 0 && writes == 1);

    reset_counts(); checked_valid = true; unchecked_valid = false;
    assert(latxs_hras_prepare_link(&cpu, &source, 0, &target));
    assert(opened == 0 && closed == 1 && writes == 0);

    reset_counts(); checked_valid = unchecked_valid = false;
    assert(!latxs_hras_prepare_link(&cpu, &source, 0, &target));
    assert(opened == 0 && closed == 0 && writes == 0);

    reset_counts(); source.jmp_cp_data[0] = 0;
    checked_valid = unchecked_valid = true;
    assert(latxs_hras_prepare_link(&cpu, &source, 0, &target));
    assert(opened == 0 && closed == 0 && writes == 0);

    reset_counts(); checked_valid = unchecked_valid = false;
    assert(!latxs_hras_prepare_link(&cpu, &source, 0, &target));
    assert(opened == 0 && closed == 0 && writes == 0);

    source.jmp_cp_data[0] = 0x105;
    reset_counts(); checked_valid = unchecked_valid = true; open_ok = false;
    assert(!latxs_hras_prepare_link(&cpu, &source, 0, &target));
    assert(opened == 1 && closed == 1 && writes == 1);
    open_ok = true;

    reset_counts(); source.bool_flags |= IS_AOT_TB;
    assert(latxs_hras_prepare_link(&cpu, &source, 0, &target));
    assert(opened == 0 && closed == 0 && writes == 0);
    source.bool_flags &= ~IS_AOT_TB;

    reset_counts(); source.is_tu = false;
    assert(latxs_hras_prepare_link(&cpu, &source, 0, &target));
    assert(opened == 0 && closed == 0 && writes == 0);
    source.is_tu = true;

    reset_counts(); next.pc = 106;
    assert(latxs_hras_prepare_link(&cpu, &source, 0, &target));
    assert(opened == 0 && closed == 0 && writes == 0);

    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tu-hras-link-policy-') as directory:
    source_file = Path(directory) / 'link.c'
    binary = Path(directory) / 'link'
    source_file.write_text(link_c)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', *shlex.split(os.environ.get('CFLAGS', '')),
                    str(source_file), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: HRAS link uses a fixed slot and patches the TUCHECK gate in place')
