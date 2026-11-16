#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Compile actual HRAS target proofs for late and ordinary JIT callees."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
translate = (root / 'target/i386/latx/translator/translate.c').read_text()
eob = (root / 'target/i386/latx/translator/sys-eob.c').read_text()
conditional_gate = (
    '        if (tu_hras_checked_call_eligible(ctx, branch, succ_id)) {\n'
    '            ctx->curr_tb->s_data->has_hras_check_gate = true;\n'
    '            la_out_nop();\n'
    '        }\n'
)
assert conditional_gate in eob


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


def last_function(source, signature):
    start = source.rindex(signature)
    return source[start:source.index('\n}', start) + 2]


c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define CONFIG_SOFTMMU 1
#define TRANSLATION_UNIT_MAX_PAGES 16
#define TARGET_PAGE_MASK (~UINT64_C(4095))
#define HF_CPL_MASK 6
#define IS_AOT_TB 1
#define IS_HRAS_CALL 0x80
#define CF_INVALID 0x40000
#define CF_NO_ACTIVE 0x80000
#define MF_MTCC_ID_MASK 0
#define MF_MTCC_ID_SHIFT 0
#define IRQ_CHECK_BLOCK_START 1
typedef struct TranslationUnitPages {
    unsigned id;
    struct { uint64_t vp_phys_hi, mix; } pages[17];
} TranslationUnitPages;
typedef struct TranslationBlock {
    uintptr_t pc, cs_base;
    unsigned flags, cflags, bool_flags, mtcc, label_pos_nocheck, identity;
    bool is_tu;
    struct { void *ptr; } tc;
    TranslationUnitPages *pages;
} TranslationBlock;
typedef struct CPUState { int unused; } CPUState;
typedef struct IR1_INST { uintptr_t target; } IR1_INST;
typedef struct ExitTBContext {
    TranslationBlock *curr_tb;
    bool allow_tb_link;
} ExitTBContext;
static bool direct_check = true, adjacent = true;
static int option_int_opt;
static bool tu_direct = true;
static bool latxs_hras_aot_translate(void) { return false; }
static TranslationBlock *canonical;
static TranslationBlock *local_target;
static TranslationBlock *tu_tree_lookup(uintptr_t pc)
{ return local_target && local_target->pc == pc ? local_target : NULL; }
static CPUState test_cpu;
static struct { CPUState *cpu_state; } environment = { &test_cpu }, *lsenv = &environment;
static uintptr_t current_site = 0x1000;
static void error_report(const char *fmt, ...) { (void)fmt; }
#define tucp_fast_direct_check() direct_check
#define tucp_fast_tu_direct() tu_direct
#define tb_is_tu(t) ((t)->is_tu)
#define TB_TU_PAGES(t) ((t)->pages)
#define tb_tu_identity(t) ((t)->identity)
#define tb_cflags(t) ((t)->cflags)
#define get_mask_from_miscflags(t,m,s) ((t)->mtcc)
#define get_current_inst_addr(t) current_site
#define env_cpu(e) (e)
#define ir1_target_addr(b) ((b)->target)
static TranslationBlock *tb_htable_lookup_noexit(CPUState *c, uintptr_t pc,
        uintptr_t cs, unsigned flags, unsigned cflags)
{ (void)c; (void)pc; (void)cs; (void)flags; (void)cflags; return canonical; }
static inline uint64_t tu_page_desc_vp(uint64_t v) { return v & TARGET_PAGE_MASK; }
static bool tu_hras_return_adjacent(TranslationBlock *t, IR1_INST *b)
{ (void)t; (void)b; return adjacent; }
'''
c += function(translate, 'static bool latxs_tucp_pages_cover(')
c += function(translate, 'static bool latxs_tucp_link_can_bypass_check(')
c += function((root / 'include/qemu/bitops.h').read_text(),
              'static inline int64_t sextract64(')
c += function(translate,
              'static void __attribute__((noreturn)) latxs_hras_link_range_error(')
c += function(translate, 'static bool latxs_hras_call_target_info(')
c += function(translate, 'bool latxs_hras_call_target_can_link(')
c += function(translate, 'bool latxs_hras_call_target_needs_long(')
c += last_function(eob, 'static bool tu_hras_checked_call_eligible(')
c += function(eob, 'static bool tu_hras_call(')
c += r'''
int main(void)
{
    TranslationUnitPages sp = {.id = 1}, tp = {0};
    for (int i = 0; i < 17; i++) {
        sp.pages[i].vp_phys_hi = tp.pages[i].vp_phys_hi = UINT64_MAX;
    }
    sp.pages[0].vp_phys_hi = tp.pages[0].vp_phys_hi = 0x1001;
    sp.pages[0].mix = tp.pages[0].mix = 123;
    sp.pages[1].vp_phys_hi = tp.pages[1].vp_phys_hi = 0x2002;
    sp.pages[1].mix = tp.pages[1].mix = 456;
    TranslationBlock source = {.pc = 0x1000, .flags = 6, .is_tu = true,
        .tc = {(void *)0x1000}, .pages = &sp, .identity = 1};
    TranslationBlock target = {.pc = 0x1010, .flags = 6,
        .tc = {(void *)0x2000}, .pages = &tp, .identity = 2};
    IR1_INST branch = {.target = target.pc};
    ExitTBContext ctx = {.curr_tb = &source, .allow_tb_link = true};
    assert(tu_hras_checked_call_eligible(&ctx, &branch, 0));
    source.bool_flags = IS_HRAS_CALL;
    assert(!tu_hras_checked_call_eligible(&ctx, &branch, 0));
    source.bool_flags = 0;
    ctx.allow_tb_link = false;
    assert(!tu_hras_checked_call_eligible(&ctx, &branch, 0));
    ctx.allow_tb_link = true;
    assert(!tu_hras_checked_call_eligible(&ctx, &branch, 1));
    adjacent = false;
    assert(!tu_hras_checked_call_eligible(&ctx, &branch, 0));
    adjacent = true;
    assert(!latxs_tucp_link_can_bypass_check(&source, &target));
    assert(latxs_hras_call_target_can_link(&source, &target, current_site, false));
    assert(!latxs_hras_call_target_needs_long(&source, &target, current_site, false));
    assert(!tu_hras_call(&source, &branch));
    tp.pages[1].mix++; /* The second ordinary instruction page also matters. */
    assert(!latxs_hras_call_target_can_link(&source, &target, current_site, false));
    assert(!tu_hras_call(&source, &branch));
    canonical = &target;
    assert(!latxs_hras_call_target_can_link(&source, &target, current_site, false));
    assert(!tu_hras_call(&source, &branch));
    canonical = NULL;
    tp.pages[1].mix--;
    tp.pages[1].vp_phys_hi = 0x3002;
    assert(!latxs_hras_call_target_can_link(&source, &target, current_site, false));
    tp.pages[1].vp_phys_hi = 0x2002;
    for (int test = 0; test < 8; test++) {
        switch (test) {
        case 0: target.bool_flags = IS_AOT_TB; break;
        case 1: target.pages = NULL; break;
        case 2: source.pages = NULL; break;
        case 3: target.cflags = CF_INVALID; break;
        case 4: target.mtcc = 1; break;
        case 5: target.flags = 0; break;
        case 6: tp.id = 3; break;
        case 7: direct_check = false; break;
        }
        assert(!latxs_hras_call_target_can_link(&source, &target,
                                                current_site, false));
        source.pages=&sp; target.pages=&tp; target.cflags=target.bool_flags=0;
        target.mtcc=0; target.flags=6; tp.id=0; direct_check=true;
    }
    target.is_tu=true;
    assert(latxs_tucp_link_can_bypass_check(&source,&target));
    assert(latxs_hras_call_target_can_link(&source,&target,current_site,false));
    assert(!latxs_hras_call_target_needs_long(&source,&target,
                                              current_site,false));
    target.tc.ptr = (void *)(current_site + 4 +
            ((UINT64_C(1) << 25) - 1) * 4);
    assert(latxs_hras_call_target_can_link(&source,&target,
                                           current_site,false));
    assert(!latxs_hras_call_target_needs_long(&source,&target,
                                              current_site,false));
    canonical=&target;
    assert(!tu_hras_call(&source,&branch));
    target.tc.ptr += 4;
    assert(latxs_hras_call_target_can_link(&source,&target,
                                           current_site,false));
    assert(latxs_hras_call_target_needs_long(&source,&target,
                                             current_site,false));
    assert(tu_hras_call(&source,&branch));
    target.is_tu=false;
    target.tc.ptr=(void *)(UINT64_C(1)<<29);
    assert(latxs_hras_call_target_can_link(&source,&target,current_site,false));
    assert(latxs_hras_call_target_needs_long(&source,&target,current_site,false));
    canonical=&target;
    assert(tu_hras_call(&source,&branch));
    TranslationBlock member = target;
    member.tc.ptr=(void *)0x3000;
    local_target=&member;
    tu_direct=false;
    assert(tu_tree_lookup(branch.target)==&member);
    assert(!tu_hras_call(&source,&branch));
    assert(latxs_hras_call_target_can_link(&source,&member,current_site,true));
    assert(!latxs_hras_call_target_needs_long(&source,&member,
                                              current_site,true));
    local_target=NULL;
    assert(tu_hras_call(&source,&branch));
    tu_direct=true;
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        target.tc.ptr=(void *)(UINT64_C(1)<<40);
        (void)latxs_hras_call_target_can_link(&source,&target,
                                              current_site,false);
        _exit(0);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    target.tc.ptr=(void *)0x2000;
    assert(!tu_hras_call(&source,&branch));
    canonical=NULL;
    branch.target=0x3010;
    assert(!tu_hras_call(&source,&branch));
    branch.target=0x1010; direct_check=false;
    assert(!tu_hras_call(&source,&branch));
    direct_check=true; source.pages=NULL;
    assert(!tu_hras_call(&source,&branch));
    source.pages=&sp; adjacent=false;
    assert(!tu_hras_call(&source,&branch));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tu-hras-call-targets-') as directory:
    source = Path(directory) / 'targets.c'
    binary = Path(directory) / 'targets'
    source.write_text(c)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', *shlex.split(os.environ.get('CFLAGS', '')),
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: boolean target predicates, gate eligibility, and page proofs')
