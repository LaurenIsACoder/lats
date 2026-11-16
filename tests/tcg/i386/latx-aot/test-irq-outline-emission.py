#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Execute the real IRQ emitter and cold fragment across normal/IRQ edges."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'target/i386/latx/translator/sys-eob.c').read_text()
env = (root / 'target/i386/latx/include/env.h').read_text()


def block(text, start):
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


labels = block(env, env.index('typedef struct HrasIndirectCallLabels')) + ' HrasIndirectCallLabels;'
hot = block(source, source.index('static bool latxs_tr_exit_tb_outline_irq(')) + '\n'
hot += block(source, source.index('static void latxs_tr_exit_tb_emit_indirect_interrupt('))
cold = block(source, source.index('    for (i = 0; indirect_calls && i < tb_num_in_tu; i++)'))
c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define HF_CPL_MASK 3
#define HF_CS64_MASK 128
#define HF_CS64_SHIFT 7
#define X86_INS_RET 9
#define X86_INS_CALL 10
#define X86_INS_JMP 11
#define IRQ_CHECK_BLOCK_END 2
#define IRQ_CHECK_ASYNC_NOTIFY 1
typedef struct { int val; } IR2_OPND;
'''+labels+r'''
typedef struct { void *ir1; int ignore_eip_update; } SideData;
typedef struct { unsigned flags; int cs_base; int int_opt[2]; SideData *s_data; } TB;
typedef TB TranslationBlock;
typedef struct { HrasIndirectCallLabels hras_indirect_call; TB *curr_tb;
    void *ir1_inst_array; int ignore_eip_update; struct { int code64; } sys; } Data;
typedef struct { TB *curr_tb; Data *tr_ctx; int branch_opcode; } ExitTBContext;
enum { LISA_LABEL, LISA_BGE, LISA_BLT, LISA_B, LOAD_IRQ, SYNC, EXIT, FSHT, HOT };
typedef struct { int op, target, succ; } Insn;
static Insn code[128];
static int nr, label, freed, option_int_opt=IRQ_CHECK_BLOCK_END;
static unsigned option_irq_outline;
static int option_tu_hras=2, in_aot_translate, option_aot_save;
static IR2_OPND la_zero={0}, ordinary_irq;
static IR2_OPND la_new_label(void) { return (IR2_OPND){++label}; }
static IR2_OPND latxs_ir2_opnd_new_label(void) { return la_new_label(); }
static IR2_OPND latxs_ra_alloc_itemp(void) { return (IR2_OPND){8}; }
static void latxs_ra_free_temp(IR2_OPND *r) { assert(r->val==8); freed++; }
static void emit(int op,int target,int succ) { assert(nr<128); code[nr++]=(Insn){op,target,succ}; }
static void latxs_load_intcheck_to_reg(IR2_OPND *r) { assert(r->val==8); emit(LOAD_IRQ,0,0); }
static bool latxs_check_interrupt_at_block_end(void) { return option_int_opt == IRQ_CHECK_BLOCK_END; }
static bool tucp_fast_fsht_check(void) { return true; }
#define la_label(l) emit(LISA_LABEL,(l).val,0)
#define la_out1(op,l) emit(op,(l)->val,0)
#define la_out3(op,a,b,l) do { assert((a)->val==8 && (b)->val==0);emit(op,(l)->val,0); } while(0)
#define latxs_append_ir2_opnd3 la_out3
#define latxs_append_ir2_opnd1(op,l) emit(op,(l)->val,0)
static IR2_OPND *latxs_ir2_interrupt_get_label(TB *tb,int succ) { (void)tb;(void)succ;return &ordinary_irq; }
static void latxs_tr_gen_intb_sync_gpc(void) { emit(SYNC,0,0); }
static void latxs_tr_gen_exit_tb_j_context_switch(void *tb,int load,int succ) {
    assert(!tb && !load); emit(EXIT,0,succ);
}
static void latxs_tr_exit_tb_emit_indirect_context_switch(const ExitTBContext *ctx,int succ) {
    (void)ctx; latxs_tr_gen_intb_sync_gpc(); latxs_tr_gen_exit_tb_j_context_switch(NULL,0,succ);
}
static void gen_latxs_intb_fsht_call_miss(int fid,bool fast,IR2_OPND *njc,IR2_OPND *miss,IR2_OPND *call) {
    assert(fid==0 && fast); (void)miss;(void)call;la_label(*njc);emit(FSHT,0,0);
}
static void gen_latxs_intb_fsht_call_miss_saved(int fid,bool fast,void *njc,void *miss,void *call,const void *regs) {
    assert(regs);gen_latxs_intb_fsht_call_miss(fid,fast,njc,miss,call);
}
static void latxs_tr_gen_indirect_call_cold(HrasIndirectCallLabels l) { (void)l;emit(FSHT,0,0); }
'''+hot+r'''
static void emit_cold(Data *td, TB *tb, HrasIndirectCallLabels *indirect_calls) {
    unsigned i, tb_num_in_tu=1; TB *tb_list[]={tb};
'''+cold+r'''
}
static void check(int negative,int succ,int expect_taken) {
    int where[128]={0};
    for(int i=0;i<nr;i++) if(code[i].op==LISA_LABEL)where[code[i].target]=i;
    int taken=0,sync=0;
    for(int ip=0,steps=0;steps<128;ip++,steps++) {
        assert(ip<nr); Insn ins=code[ip];
        if(ins.op==LISA_BGE || ins.op==LISA_BLT || ins.op==LISA_B) {
            bool jump=ins.op==LISA_B || (ins.op==LISA_BLT ? negative : !negative);
            if(jump) { taken++;ip=where[ins.target]; }
        } else if(ins.op==SYNC) sync++;
        else if(ins.op==EXIT) { assert(negative && sync==1 && ins.succ==succ);return; }
        else if(ins.op==HOT) { assert(!negative && !sync && taken==expect_taken);return; }
        else assert(ins.op!=FSHT);
    }
    assert(0);
}
int main(void) {
    unsigned checks=0;
    for(unsigned mode=0;mode<=3;mode++)
    for(int inline_tu=0;inline_tu<=1;inline_tu++)
    for(int fsht=-1;fsht<=0;fsht++)
    for(int succ=0;succ<=1;succ++)
    for(int call=0;call<=1;call++)
    for(int scope=0;scope<7;scope++) {
        if(fsht>=0 && (!call || scope))continue;
        nr=label=freed=0;option_irq_outline=mode;
        option_tu_hras=scope==1?0:2;in_aot_translate=scope==2;option_aot_save=scope==5;
        SideData sd={0};TB tb={.flags=scope==3?3:scope==4?128:131,.cs_base=scope==6,.s_data=&sd};
        Data data={0};ExitTBContext ctx={&tb,&data,call?X86_INS_CALL:X86_INS_JMP};
        HrasIndirectCallLabels *l=&data.hras_indirect_call;
        l->valid=call && !scope;
        l->miss=la_new_label();l->call=la_new_label();
        ordinary_irq=la_new_label();IR2_OPND miss=la_new_label();
        latxs_tr_exit_tb_emit_indirect_interrupt(&ctx,succ,inline_tu,call?&miss:NULL,inline_tu?fsht:-1);
        assert(freed==1);emit(HOT,0,0);
        emit_cold(&data,&tb,l);
        if(!inline_tu) {la_label(ordinary_irq);latxs_tr_exit_tb_emit_indirect_context_switch(&ctx,succ);}
        bool deferred=!!(mode & (call?2:1)) && inline_tu && !scope;
        assert(l->irq_deferred==deferred);
        if(deferred) {
            assert(l->irq_succ_id==(unsigned)succ);
            if(fsht>=0)assert(l->fsht_deferred && miss.val==l->fsht.val);
            else { assert(!l->fsht_deferred);if(call)assert(miss.val==l->miss.val); }
        }
        check(0,succ,inline_tu && !deferred);check(1,succ,0);checks+=2;
    }
    printf("PASS: %u real IRQ control-flow cases; normal fallthrough and pending-IRQ target sync\n",checks);
}
'''
with tempfile.TemporaryDirectory(prefix='tu-hras-cold-irq-') as tmp:
    path = Path(tmp)
    (path / 'irq.c').write_text(c)
    binary = path / 'irq'
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                    '-Wall', '-Wextra', '-Werror', str(path / 'irq.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
