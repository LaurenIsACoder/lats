#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Execute the real AOT TU backedge emitter and verify the linked branch slot."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
source = (root / 'target/i386/latx/translator/sys-tr-misc.c').read_text()
def block(prefix):
    start = source.index(prefix);end = source.index('{', start)+1;depth=1
    while depth:
        depth += (source[end]=='{') - (source[end]=='}');end+=1
    return source[start:end]
helper=block('typedef enum TUBackedgeIRQPath')+' TUBackedgeIRQPath;\n'
helper+=block('static TUBackedgeIRQPath latxs_tu_backedge_irq_path(')+'\n'
helper+=block('static void latxs_tu_backedge_irq_exit(')
func=block('static bool latxs_tu_translate_jmp(')
c=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define CONFIG_LATX_TU 1
#define IRQ_CHECK_BLOCK_END 2
#define IRQ_CHECK_ASYNC_NOTIFY 3
#define TU_TB_INDEX_TARGET 1
#define TU_COLD_EXIT_DIRECT_PENDING 1
#define TU_COLD_EXIT_JMP_INTERRUPT_DEFERRED 2
#define TB_JMP_RESET_OFFSET_INVALID 0xffffffffu
#define TARGET_PAGE_MASK (~UINT64_C(4095))
#define HF_CS64_MASK 128
#define HF_CPL_MASK 3
#define g_assert assert
#define LOAD_GUEST_ADDR 100
#define CS_NATIVE_TO_BT_RET_0 101
#define ADDR uintptr_t
#define ADDRX uint64_t
enum {LISA_BGE,LISA_BLT,LISA_B,LISA_LABEL,LISA_ST_D,LISA_ORI,
      LOAD,LOOP,TBADDR,GUESTADDR,CONTEXT,NOP};
typedef struct {int val;} IR2_OPND;
typedef struct {uint64_t pc,target;} IR1_INST;
typedef struct TB TB;
typedef struct {TB *next_tb[2];unsigned tu_cold_exit_offset[2],tu_jmp[2];} Side;
struct TB {Side *s_data;uint64_t pc,flags,cs_base;struct {void *ptr;} tc;};
typedef TB TranslationBlock;
typedef struct {TB *curr_tb;bool hras_long_checks,irq_backedge_short;
    unsigned ir2_asm_nr,mtcc_id;uintptr_t tcg_region_offset[2];} TRANSLATION_DATA;
static TRANSLATION_DATA data;static struct {TRANSLATION_DATA *tr_data;} env={&data},*lsenv=&env;
static IR2_OPND la_zero={0},la_env={23},latxs_zero_ir2_opnd={0},int_label={1};
static unsigned option_int_opt,option_irq_backedge,option_tu_hras;
static bool in_aot_translate,option_aot_save,adjacent,contained,aot_tu=true;
typedef struct {int op,label;uint64_t payload;unsigned off;} Insn;
static Insn code[32];static int nr,next_label,freed;static uint64_t loaded_eip;
static void emit(int op,int label,uint64_t payload) {
    assert(nr<32);code[nr++]=(Insn){op,label,payload,data.ir2_asm_nr*4};
    if(op!=LISA_LABEL)data.ir2_asm_nr++;
}
static bool aot_tu_enabled(void){return aot_tu;}
static bool latxs_check_interrupt_at_block_end(void){return option_int_opt==IRQ_CHECK_BLOCK_END || option_int_opt==IRQ_CHECK_ASYNC_NOTIFY;}
static bool tb_is_in_tb_list(TB *tb){(void)tb;return contained;}
static bool tb_is_adjacent(TB *a,TB *b){(void)a;(void)b;return adjacent;}
static bool tucp_fast_tu_direct(void){return true;}
static int64_t tb_page_addr1(TB *tb){(void)tb;return -1;}
static uint64_t ir1_addr(IR1_INST *x){return x->pc;}
static uint64_t ir1_target_addr(IR1_INST *x){return x->target;}
static IR2_OPND la_new_itmp(void){return (IR2_OPND){8};}
static IR2_OPND la_new_label(void){return (IR2_OPND){++next_label};}
static void latxs_load_intcheck_to_reg(IR2_OPND *tmp){assert(tmp->val==8);emit(LOAD,0,0);}
static IR2_OPND *latxs_ir2_interrupt_get_label(TB *tb,int id){(void)tb;assert(id==1);return &int_label;}
static IR2_OPND latxs_ra_alloc_dbt_arg1(void){return (IR2_OPND){5};}
static void latxs_tr_gen_exit_tb_load_tb_addr(IR2_OPND *r,ADDR tb){assert(r->val==5);emit(TBADDR,0,tb);}
static void aot_load_guest_addr_to_ir2(IR2_OPND *r,ADDRX eip,int kind){assert(r->val==8&&kind==LOAD_GUEST_ADDR);loaded_eip=eip;emit(GUESTADDR,0,eip);}
static void latxs_load_imm64_to_ir2(IR2_OPND *r,ADDRX eip){assert(r->val==8);loaded_eip=eip;emit(GUESTADDR,0,eip);}
static int lsenv_offset_of_eip(void *p){(void)p;return 88;}
static uintptr_t get_current_inst_addr(TB *tb){return (uintptr_t)tb->tc.ptr+data.ir2_asm_nr*4;}
static void tu_jcc_nop_gen(TB *tb,int n,bool target){(void)tb;assert(n==1&&target);emit(LOOP,0,0);}
#define la_out3(op,a,b,l) do{assert((a)->val==8&&(b)->val==0);emit(op,(l)->val,0);}while(0)
#define la_out1(op,l) emit(op,(l)->val,0)
#define la_label(l) emit(LISA_LABEL,(l).val,0)
#define la_out2i(op,a,b,off) do{assert((a)->val==8&&(b)->val==23&&(off)==88);emit(op,0,loaded_eip);}while(0)
#define tb_sc_table_offset_jmp(tb,rid,name,region,off,link,kind) do{ \
 assert((tb)==data.curr_tb && (rid)==0 && (off)==(int)data.ir2_asm_nr*4 && !(link) && (kind)==CS_NATIVE_TO_BT_RET_0);emit(CONTEXT,0,(uintptr_t)(tb)); \
}while(0)
#define latxs_append_ir2_opnd2i(op,a,b,off) do{(void)(op);(void)(a);(void)(b);(void)(off);emit(NOP,0,0);}while(0)
#define la_tmp_free(x) do{assert((x).val==8);freed++;}while(0)
''' + helper + '\n' + func + r'''
static void check_path(int pending,bool expect_irq, uint64_t eip,TB *tb) {
    int where[32]={0};for(int i=0;i<nr;i++)if(code[i].op==LISA_LABEL)where[code[i].label]=i;
    int seen_store=0;
    for(int pc=0,steps=0;steps<32;pc++,steps++){
        assert(pc<nr);Insn ins=code[pc];
        if(ins.op==LISA_BGE||ins.op==LISA_BLT||ins.op==LISA_B){
            bool jump=ins.op==LISA_B||(ins.op==LISA_BLT?pending:!pending);
            if(jump){assert(where[ins.label]);pc=where[ins.label];}
        }else if(ins.op==LISA_ST_D){seen_store++;assert(ins.payload==eip);}
        else if(ins.op==CONTEXT){assert(expect_irq&&seen_store==1&&ins.payload==(uintptr_t)tb);return;}
        else if(ins.op==LOOP||ins.op==NOP){assert(!expect_irq&&!seen_store);return;}
    }
    assert(0);
}
int main(void){
    unsigned cases=0;
    for(int enabled=0;enabled<2;enabled++)for(int translating=0;translating<2;translating++)
    for(int saving=0;saving<2;saving++)for(int adj=0;adj<2;adj++)
    for(int tu=0;tu<2;tu++)for(int scope=0;scope<4;scope++)for(int pending=0;pending<2;pending++){
        Side side={0};TB target={.pc=0x4000},tb={.s_data=&side,.pc=0x5000,.flags=131,.tc.ptr=(void*)0x10000};
        if(scope==1)tb.flags=3;else if(scope==2)tb.flags=128;else if(scope==3)tb.cs_base=8;
        side.next_tb[1]=&target;side.tu_cold_exit_offset[1]=0;data=(TRANSLATION_DATA){.curr_tb=&tb};
        IR1_INST guest={.pc=tb.pc,.target=target.pc};option_int_opt=2;option_tu_hras=2;option_irq_backedge=enabled;in_aot_translate=translating;option_aot_save=saving;adjacent=adj;contained=true;aot_tu=tu;
        nr=freed=0;next_label=1;assert(latxs_tu_translate_jmp(&guest));assert(freed==1);
        bool changed=enabled&&translating&&saving&&!adj&&tu&&!scope;
        assert(code[0].op==LOAD && code[1].op==(changed?LISA_BLT:LISA_BGE));
        if(changed){assert(code[2].op==LOOP&&code[3].op==LISA_LABEL);assert(side.tu_jmp[1]==code[2].off);assert(code[3].off-code[1].off==8);}
        check_path(pending,pending,guest.target,&tb);cases++;
    }
    { /* A forward TU jump does not gain an IRQ poll. */
        Side side={0};TB target={.pc=0x6000},tb={.s_data=&side,.pc=0x5000,.flags=131,.tc.ptr=(void*)0x10000};
        side.next_tb[1]=&target;data=(TRANSLATION_DATA){.curr_tb=&tb};IR1_INST guest={.pc=tb.pc,.target=target.pc};
        option_int_opt=2;option_irq_backedge=1;in_aot_translate=option_aot_save=aot_tu=contained=true;adjacent=false;
        nr=freed=0;next_label=1;assert(latxs_tu_translate_jmp(&guest));assert(nr==1&&code[0].op==LOOP&&freed==0);cases++;
        /* An edge outside the TU remains a normal non-TU exit. */
        contained=false;nr=0;assert(!latxs_tu_translate_jmp(&guest));assert(!nr&&side.tu_jmp[1]==TB_JMP_RESET_OFFSET_INVALID);cases++;
    }
    printf("PASS: %u real AOT backedge control-flow, eligibility, jump-offset and near-label cases\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='aot-irq-backedge-') as d:
    p=Path(d);(p/'test.c').write_text(c)
    subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
