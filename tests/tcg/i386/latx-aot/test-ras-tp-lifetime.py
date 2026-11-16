#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise queued-link expiry with deliberately unreadable TB pointers."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/optimization/sys-ras.c").read_text()


def function(signature):
    begin = source.index(signature)
    return source[begin:source.index("\n}", begin) + 2]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#define CONFIG_RAS_SLOW 1
#define unlikely(v) (v)
#define qatomic_read(p) (*(p))
#define qatomic_set(p,v) (*(p)=(v))
#define qatomic_xchg(p,v) __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST)
#define g_free free
struct state {
    uint32_t reset_pending, top;
    bool link_queued;
    uint64_t expired_links, repaired_links;
};
typedef struct state LatxRasTPState;
typedef struct state Soft_RAS;
typedef struct { struct state ras_tp; } CPUX86State;
typedef struct { CPUX86State *env_ptr; Soft_RAS *s_ras; bool owner; } CPUState;
#define tb_cflags(tb) ((tb)->cflags)
#define CF_INVALID 0x40000
#define MF_RAS_CLEAR_B0 0x400
#define MF_RAS_LINKED 0x800
#define MF_MTCC_ID_MASK 0x0f000000
#define MF_MTCC_ID_SHIFT 24
typedef struct TranslationBlock { int valid,ir2_num,flags,cflags,inst_ptr_offset,misc_flags; struct {void *ptr;} tc; struct TranslationBlock *preorder_next; uintptr_t ras_dest; } TranslationBlock;
static bool qemu_tcg_mtcc_enabled(void) { return true; }
static int get_mask_from_miscflags(TranslationBlock *tb,int mask,int shift) {return (tb->misc_flags&mask)>>shift;}
static bool latxs_ras_flag_is_set(TranslationBlock *tb,int flag) {return tb->misc_flags&flag;}
static int disabled;
static void latxs_ras_unlink(TranslationBlock *tb) {tb->misc_flags|=MF_RAS_CLEAR_B0;tb->preorder_next=NULL;tb->ras_dest=1;disabled++;}
#define g_assert assert
static void remove_misc_flags(TranslationBlock *tb,int flags) {tb->misc_flags&=~flags;}
#define TB_READY {.valid=1,.ir2_num=1,.flags=1,.inst_ptr_offset=24,.misc_flags=1<<24,.tc={.ptr=(void *)16}}

typedef struct { void *host_ptr; } run_on_cpu_data;
typedef struct { TranslationBlock *source,*target; uint32_t full_count,mtcc_count,mtcc; } RasTPLinkRequest;
static struct { uint32_t tb_flush_count, tb_flush_count_mtcc[2]; } tb_ctx;
static int option_rastoptp=1, links;
static bool link_result;
static bool qemu_cpu_is_self(CPUState *cpu) { return cpu->owner; }
static bool ras_link_now(TranslationBlock *source, TranslationBlock *target) {
    assert(source->valid && target->valid);
    links++;
    return link_result;
}
'''
checks = r'''
int main(void) {
    CPUX86State env={0}; CPUState cpu={&env,&env.ras_tp,false};
    TranslationBlock source=TB_READY,target=TB_READY;
    tb_ctx.tb_flush_count=5; tb_ctx.tb_flush_count_mtcc[1]=7;
    for (int mismatch=1;mismatch<=3;mismatch++) {
        RasTPLinkRequest *request=malloc(sizeof(*request));
        *request=(RasTPLinkRequest){(void *)1,(void *)3,5-!!(mismatch&1),7-!!(mismatch&2),1};
        env.ras_tp.link_queued=true;
        ras_tp_finish_link(&cpu,(run_on_cpu_data){request});
        assert(!env.ras_tp.link_queued && env.ras_tp.expired_links==(unsigned)mismatch);
        assert(!links && !env.ras_tp.repaired_links);
    }
    for (int success=0;success<=1;success++) {
        RasTPLinkRequest *request=malloc(sizeof(*request));
        *request=(RasTPLinkRequest){&source,&target,5,7,1};
        env.ras_tp.link_queued=true;link_result=success;
        ras_tp_finish_link(&cpu,(run_on_cpu_data){request});
        assert(!env.ras_tp.link_queued && links==success+1);
        assert(env.ras_tp.repaired_links==(unsigned)success);
    }
    target.ir2_num=0;
    assert(ras_tp_empty_target(&source,&target));
    RasTPLinkRequest *empty=malloc(sizeof(*empty));
    *empty=(RasTPLinkRequest){&source,&target,5,7,1};
    ras_tp_finish_link(&cpu,(run_on_cpu_data){empty});
    assert(disabled==1 && links==2 && !env.ras_tp.link_queued);
    assert(source.misc_flags&MF_RAS_CLEAR_B0);
    source.misc_flags&=~MF_RAS_CLEAR_B0;
    target.cflags=CF_INVALID;assert(!ras_tp_empty_target(&source,&target));
    target.cflags=0;target.flags=2;assert(!ras_tp_empty_target(&source,&target));
    target.flags=1;target.misc_flags=0;assert(!ras_tp_empty_target(&source,&target));
    target.misc_flags=1<<24;target.tc.ptr=NULL;assert(!ras_tp_empty_target(&source,&target));
    target.tc.ptr=(void *)16;target.ir2_num=1;assert(!ras_tp_empty_target(&source,&target));
    assert(!ras_tp_empty_target(&source,NULL));
    source.misc_flags|=MF_RAS_CLEAR_B0;source.ras_dest=1;
    target.misc_flags|=MF_RAS_LINKED;target.ras_dest=(uintptr_t)&source;target.preorder_next=&source;
    TranslationBlock *retry_list[]={&source,&target};
    tu_reset_private_ras(2,retry_list);
    assert(!(source.misc_flags&(MF_RAS_CLEAR_B0|MF_RAS_LINKED)) && !source.ras_dest);
    assert(!(target.misc_flags&(MF_RAS_CLEAR_B0|MF_RAS_LINKED)) && !target.ras_dest && !target.preorder_next);
    env.ras_tp.top=123;latxs_flush_soft_ras(&cpu);
    assert(env.ras_tp.top==123 && env.ras_tp.reset_pending==1);
    cpu.owner=true;latxs_flush_soft_ras(&cpu);
    assert(!env.ras_tp.top && env.ras_tp.reset_pending==1);
    uint64_t frame[32]={0};frame[2]=123;env.ras_tp.top=123;
    latxs_ras_tp_redirect(&env,frame);
    assert(!env.ras_tp.top && !env.ras_tp.reset_pending && !frame[2]);
    env.ras_tp.top=123;frame[2]=99;
    latxs_ras_tp_redirect(&env,frame);
    assert(env.ras_tp.top==123 && frame[2]==123);
    option_rastoptp=0;frame[2]=123;env.ras_tp.top=123;
    latxs_ras_tp_redirect(&env,frame);
    assert(env.ras_tp.top==123 && frame[2]==123);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / "check.c"
    tu_source=(root/"target/i386/latx/optimization/sys-tu.c").read_text()
    tu_start=tu_source.index("static void tu_reset_private_ras(")
    tu_reset=tu_source[tu_start:tu_source.index("\n}",tu_start)+2]
    path.write_text(prefix + tu_reset + function("static bool ras_tp_empty_target(") + function("static void ras_tp_finish_link(") +
                    function("void latxs_flush_soft_ras(") +
                    function("void latxs_ras_tp_redirect(") + checks)
    binary = Path(directory) / "check"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
patch_prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define CONFIG_LATX_TU 1
#define CONFIG_RAS_SLOW 1
#define RAS_SLOW_UNLINK_BRANCH_INSNS 20
#define LATX_RAS_TP_CALL_SKIP (0x50000000U | (7U<<10))
#define MF_RAS_CLEAR_B0 0x400
#define MF_RAS_LINKED 0x800
#define qatomic_read(p) (*(p))
#define qatomic_set(p,v) (*(p)=(v))
#define qatomic_or(p,v) (*(p)|=(v))
#define qatomic_and(p,v) (*(p)&=(v))
#define g_assert(v) assert(v)
#define LATXS_RAS_TP_COLD_OFFSET(tb) ((tb)->cold)
#define tb_is_tu(tb) ((tb)->tu)
typedef struct {struct {void *ptr;uint32_t offset_in_tu;} tc;uint32_t inst_ptr_offset,cold,ir2_num,misc_flags;bool tu;} TranslationBlock;
static int option_rastoptp=1;
static void flush_idcache_range(uintptr_t start,uintptr_t end,int size) {(void)start;(void)end;assert(size==4);}
static void latxs_ras_ibar(void) {}
'''
patch_checks = r'''
int main(void) {
    for (int tu=0;tu<2;tu++) {
        uint32_t code[96],before[96];for(int i=0;i<96;i++)code[i]=0x03400000+i;
        TranslationBlock tb={.tc={.ptr=code+tu*4,.offset_in_tu=tu*16},.inst_ptr_offset=32,.cold=128,.ir2_num=32,.misc_flags=MF_RAS_LINKED,.tu=tu};
        memcpy(before,code,sizeof(code));
        latxs_ras_patch_permanent(&tb);
        assert(code[5]==LATX_RAS_TP_CALL_SKIP);
        assert(code[8]==latxs_ras_tp_closed_gate(&tb));
        assert((tb.misc_flags&(MF_RAS_CLEAR_B0|MF_RAS_LINKED))==MF_RAS_CLEAR_B0);
        for (int i=0;i<96;i++)if(i!=5 && i!=8)assert(code[i]==before[i]);
        assert(latxs_ras_tp_permanent_valid(&tb));
        code[5]=before[5];assert(!latxs_ras_tp_permanent_valid(&tb));
        code[5]=LATX_RAS_TP_CALL_SKIP;tb.ir2_num=3;assert(!latxs_ras_tp_permanent_valid(&tb));
        tb.ir2_num=32;tb.misc_flags|=MF_RAS_LINKED;assert(!latxs_ras_tp_permanent_valid(&tb));
    }
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)/"patch.c"
    definitions="".join(function(signature) for signature in (
        "static inline uint32_t *latxs_ras_inst_ptr(",
        "static inline bool latxs_ras_flag_is_set(",
        "static inline void latxs_ras_set_flag(",
        "static inline void latxs_ras_clear_flag(",
        "uint32_t latxs_ras_tp_closed_gate(",
        "static void latxs_ras_patch_detach(",
        "static void latxs_ras_patch_permanent(",
        "bool latxs_ras_tp_permanent_valid("))
    path.write_text(patch_prefix+definitions+patch_checks)
    binary=Path(directory)/"patch"
    subprocess.run(["cc","-std=gnu11","-Wall","-Wextra","-Werror",str(path),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print("PASS: expired links never read reclaimed TBs; remote flush preserves owner top")
