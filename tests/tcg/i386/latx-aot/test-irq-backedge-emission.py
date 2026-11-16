#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Execute the actual direct-backedge IRQ fragment, including long retry."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
s = (root / 'target/i386/latx/translator/sys-tr-misc.c').read_text()
def block(prefix):
    start = s.index(prefix);end = s.index('{', start)+1;depth=1
    while depth:
        depth += (s[end]=='{') - (s[end]=='}');end+=1
    return s[start:end]
helper = block('typedef enum TUBackedgeIRQPath')+' TUBackedgeIRQPath;\n'
helper += block('static TUBackedgeIRQPath latxs_tu_backedge_irq_path(')
start = s.index('            case TU_IRQ_COLD_SHORT:')
end = s.index('            case TU_IRQ_INLINE_EXIT:', start)
c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
enum {HF_CS64_MASK=128,HF_CPL_MASK=3,TU_TB_INDEX_TARGET=1,
      TU_COLD_EXIT_DIRECT_PENDING=10,TU_COLD_EXIT_JMP_INTERRUPT_DEFERRED=11,
      LISA_BLT,LISA_BGE,LISA_B,LISA_LABEL,LOAD};
typedef struct {int val;} IR2_OPND;
typedef struct {unsigned tu_cold_exit_offset[2];} Side;
typedef struct {Side *s_data;unsigned flags,cs_base;} TB;
typedef struct {bool hras_long_checks,irq_backedge_short;} Data;
typedef TB TranslationBlock;
typedef Data TRANSLATION_DATA;
static bool aot_tu_enabled(void){return false;}
static bool tb_is_adjacent(TB*a,TB*b){(void)a;(void)b;return false;}
static unsigned option_irq_backedge,option_tu_hras,in_aot_translate,option_aot_save;
static IR2_OPND la_zero={0},irq={1};
typedef struct {int op,label;} Insn;
static Insn code[8];static int nr,labels;
static void emit(int op,int label) {assert(nr<8);code[nr++]=(Insn){op,label};}
static IR2_OPND la_new_label(void) {return (IR2_OPND){++labels};}
static void latxs_load_intcheck_to_reg(IR2_OPND *r) {assert(r->val==8);emit(LOAD,0);}
static IR2_OPND *latxs_ir2_interrupt_get_label(TB *tb,int n) {(void)tb;assert(n==1);return &irq;}
#define la_out3(op,a,b,l) do{assert((a)->val==8 && (b)->val==0);emit(op,(l)->val);}while(0)
#define la_out1(op,l) emit(op,(l)->val)
'''+helper+r'''
static void fragment(TB *tb,Data *td) {
    IR2_OPND tmp={8};
    TUBackedgeIRQPath path=latxs_tu_backedge_irq_path(tb,NULL,td);
    switch(path) {
'''+s[start:end]+r'''
    default: assert(0);
    }
}
int main(void) {
    unsigned checks=0;
    for(unsigned enabled=0;enabled<2;enabled++)
    for(unsigned long_form=0;long_form<2;long_form++)
    for(unsigned scope=0;scope<7;scope++) {
        option_irq_backedge=enabled;option_tu_hras=scope==1?0:2;
        in_aot_translate=scope==2;option_aot_save=scope==3;
        Side side={{0,TU_COLD_EXIT_DIRECT_PENDING}};
        TB tb={&side,scope==4?3:scope==5?128:131,scope==6};
        Data data={long_form,false};nr=0;labels=1;
        fragment(&tb,&data);
        bool short_form=enabled && !long_form && !scope;
        assert(data.irq_backedge_short==short_form);
        assert(side.tu_cold_exit_offset[1]==TU_COLD_EXIT_JMP_INTERRUPT_DEFERRED);
        assert(code[0].op==LOAD && nr==(short_form?2:4));
        for(unsigned pending=0;pending<2;pending++) {
            int taken=0,loads=0;bool exit_irq=false;
            for(int pc=0;pc<nr;pc++) {
                Insn ins=code[pc];
                if(ins.op==LOAD) {loads++;continue;}
                bool jump=ins.op==LISA_B ||
                    (ins.op==LISA_BLT && pending) ||
                    (ins.op==LISA_BGE && !pending);
                if(!jump)continue;
                taken++;
                if(ins.label==1) {exit_irq=true;break;}
                bool found=false;
                for(int j=0;j<nr;j++)if(code[j].op==LISA_LABEL && code[j].label==ins.label) {
                    pc=j;found=true;break;
                }
                assert(found);
            }
            assert(loads==1 && exit_irq==!!pending);
            assert(taken==(pending?1:!short_form));checks++;
        }
    }
    printf("PASS %u direct IRQ edge/scope/long-retry cases\n",checks);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(c)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-Wall','-Wextra','-Werror']+
                   shlex.split(os.environ.get('CFLAGS',''))+[str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
