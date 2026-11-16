#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise actual production RET eligibility, preservation, fallback and load fault."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[4]
s=(root/'target/i386/latx/optimization/sys-push-pop.c').read_text()
helper=s[s.index('static bool ret_ends_64bit_pushpop_block('):s.index('void latx_restore_sp(')]
r=(root/'target/i386/latx/translator/sys-tr-misc.c').read_text()
ret=r[r.index('bool latxs_translate_ret('):r.index('bool latxs_translate_cpuid(')]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <setjmp.h>
#define lsassert assert
#define EXMode_Z 0
#define lisa_mov 0
#ifdef TEST_WITH_TU
#define CONFIG_LATX_TU 1
#endif
#define CONFIG_SOFTMMU 1
#define INSTPTN_OPC_NONE 0
#define LISA_ADDI_D 1
#define g_assert assert
static unsigned esp_index=4;static bool option_pushpop=true,context=true;
static bool option_pushpop_ret=true,option_sys_aot,option_aot_save,in_aot_translate,hamt=true;
enum {X86_INS_ADD,X86_INS_SUB,X86_INS_POP,X86_INS_RET,X86_INS_NOP,X86_INS_INC,X86_INS_DEC,X86_INS_NOT,X86_INS_NEG,X86_INS_ENTER};
typedef struct {int kind,width,reg;int64_t imm;struct {int base;} mem;} IR1_OPND;
typedef struct {unsigned opc;} Pattern;
typedef struct {int opcode,n,width,flags,prefix;uint64_t pc;int size;Pattern instptn;IR1_OPND op[2];struct {unsigned id;} *info;} IR1_INST;
typedef struct {int sys_eob_ir1_index,tb_too_large_ir1_index;} Data;
typedef struct {Data *s_data;int icount;} TranslationBlock;
typedef struct {int sp_patch,ir1_nr,curr_ir1_nr;bool is_tu_mode;void *curr_tb;IR1_INST *ir1_inst_array;struct {int popl_esp_hack;bool code64;unsigned flags;} sys;} TRANSLATION_DATA;
static TRANSLATION_DATA td;static struct {TRANSLATION_DATA *tr_data;void *cpu_state;} env={&td,0},*lsenv=&env;
static bool hamt_enabled(void){return hamt;}
static bool can_do_hamt(void *p){return true;}
static int stack_width=8;static int latxs_get_sys_stack_addr_size(void){return stack_width;}
static unsigned ir1_opcode(IR1_INST *i){return i->opcode;}
static int ir1_opnd_num(IR1_INST *i){return i->n;}
static int latxs_ir1_data_size(IR1_INST *i){return i->width;}
#define PREFIX(name,bit) static bool latxs_ir1_has_prefix_##name(IR1_INST *i){return i->prefix&bit;}
PREFIX(lock,1) PREFIX(opsize,2) PREFIX(addrsize,4) PREFIX(rep,8) PREFIX(repne,16) PREFIX(seg,32)
static uint64_t ir1_addr(IR1_INST *i){return i->pc;}
static uint64_t ir1_addr_next(IR1_INST *i){return i->pc+i->size;}
static IR1_OPND *ir1_get_opnd(IR1_INST *i,int n){return &i->op[n];}
static bool ir1_opnd_is_gpr(IR1_OPND *o){return o->kind==1;}
static bool ir1_opnd_is_imm(IR1_OPND *o){return o->kind==2;}
static unsigned ir1_opnd_base_reg_num(IR1_OPND *o){return o->reg;}
static int ir1_opnd_size(IR1_OPND *o){return o->width;}
static int64_t ir1_opnd_simm(IR1_OPND *o){return o->imm;}
static bool ir1_need_calculate_any_flag(IR1_INST *i){return i->flags;}
static int saved[16],emissions;static int64_t emitted_delta;
static int *tb_opt_ptr(TranslationBlock *tb){return saved;}
typedef int IR2_OPND;
static IR2_OPND latxs_ra_alloc_gpr(unsigned r){return r;}
static void la_out2i(int op,IR2_OPND *d,IR2_OPND *s,int64_t imm){assert(op==LISA_ADDI_D);emissions++;emitted_delta=imm;}
''' +r'''
static bool option_soft_ras=true,load_fault;
static IR2_OPND latxs_arg1_ir2_opnd;
static int events[8],event_count,load_offset;static jmp_buf fault;
static uint64_t ir1_opnd_uimm(IR1_OPND *o){return o->imm;}
#define HF_CPL_MASK 3
#define HF_INHIBIT_IRQ_MASK (1 << 3)
#define X86_REG_INVALID 0
#define X86_REG_SS 1
#define X86_REG_RSP 4
#define X86_REG_ESP 5
#define X86_REG_SP 6
#define X86_REG_SPL 7
static bool ir1_opnd_is_mem(IR1_OPND *o){return o->kind==3;}
static void ir1_opnd_build_full_mem(IR1_OPND *o,int w,int seg,int base,int offset,int index,int scale){o->imm=offset;}
static IR2_OPND latxs_ra_alloc_itemp(void){return 0;}
static void latxs_load_ir1_mem_to_ir2(IR2_OPND *dst,IR1_OPND *mem,int mode,int width){
 events[event_count++]=1;load_offset=mem->imm;if(load_fault)longjmp(fault,1);
}
static void latxs_update_stack_pointer(int delta,int width){events[event_count++]=2;emissions++;emitted_delta=delta;}
static void latxs_append_ir2_opnd2_(int op,IR2_OPND *a,IR2_OPND *b){}
static void latxs_ra_free_temp(IR2_OPND *a){}
static void latxs_translate_ras_pop(IR2_OPND *a){events[event_count++]=3;}
static void latxs_tr_generate_exit_tb(IR1_INST *i,int exit){events[event_count++]=4;}
''' +helper+ret+r'''
static IR1_INST ir[4];static Data data;static TranslationBlock tb={.s_data=&data};
static void setup(int pending){
 memset(ir,0,sizeof(ir));memset(saved,0,sizeof(saved));
 emissions=event_count=0;load_fault=false;load_offset=0;
 option_pushpop=option_pushpop_ret=hamt=true;option_sys_aot=option_aot_save=in_aot_translate=false;stack_width=8;
 data=(Data){-1,-1};tb.icount=4;
 td=(TRANSLATION_DATA){.sp_patch=pending,.ir1_nr=4,.curr_ir1_nr=3,.is_tu_mode=true,.curr_tb=&tb,.ir1_inst_array=ir};
 td.sys.code64=true;td.sys.flags=3;
 for(int i=0;i<4;i++){static struct {unsigned id;} info;ir[i].info=(void *)&info;ir[i].pc=0x1000+i;ir[i].size=1;ir[i].width=64;}
 ir[3].opcode=X86_INS_RET;
}
int main(void){
 unsigned checks=0;
 for(int pending=-2050;pending<=2049;pending++){
  setup(pending);bool eligible=pending!=0&&pending>=-2048&&pending<=2039;
  assert(latxs_ret_can_consume_sp(ir+3)==eligible);
  assert(!latx_adjust_sp_before_translate(&tb,ir+3,3));
  assert(saved[3]==(eligible?pending:0));
  assert(td.sp_patch==(eligible?pending:0));
  int hook_emissions=emissions;event_count=0;emissions=0;
  assert(latxs_translate_ret(ir+3));
  assert(load_offset==(eligible?pending:0));
  assert(emitted_delta==(eligible?pending+8:8));
  assert(td.sp_patch==0);
  assert(event_count==4&&events[0]==1&&events[1]==2&&events[2]==3&&events[3]==4);
  assert(hook_emissions==(!eligible&&pending!=0));checks++;
 }
 for(int bad=0;bad<10;bad++){
  setup(48);
  if(bad==0)ir[3].prefix=1;if(bad==1)ir[3].n=1;
  if(bad==2){ir[3].prefix=2;ir[3].width=16;}
  if(bad==3)ir[3].opcode=X86_INS_NOP;if(bad==4)ir[3].instptn.opc=1;
  if(bad==5){td.sys.code64=false;stack_width=4;}
  if(bad==6)td.sys.flags=0;if(bad==7)option_pushpop_ret=false;
  if(bad==8)td.curr_ir1_nr=2;if(bad==9)option_pushpop=false;
  assert(!latxs_ret_can_consume_sp(ir+3));
  if(bad!=6&&bad!=9){assert(!latx_adjust_sp_before_translate(&tb,ir+3,3));assert(td.sp_patch==0&&saved[3]==0&&emissions==1);}
  checks++;
 }
 /* Prefixes use the decoded data width; 66 with REX.W remains 64-bit. */
 for(int prefix=2;prefix<=32;prefix<<=1)for(int pending=-48;pending<=48;pending+=96)for(int direct=0;direct<2;direct++){
  setup(pending);ir[3].prefix=prefix;hamt=direct;
  assert(latxs_ret_can_consume_sp(ir+3));
  assert(!latx_adjust_sp_before_translate(&tb,ir+3,3));
  assert(saved[3]==pending&&td.sp_patch==pending&&!emissions);
  assert(latxs_translate_ret(ir+3));
  assert(load_offset==pending&&emitted_delta==pending+8&&td.sp_patch==0);
  assert(event_count==4&&events[0]==1&&events[1]==2&&events[2]==3&&events[3]==4);
  setup(pending);ir[3].prefix=prefix;hamt=direct;
  assert(!latx_adjust_sp_before_translate(&tb,ir+3,3));load_fault=true;
  if(!setjmp(fault)){latxs_translate_ret(ir+3);assert(0);}
  assert(event_count==1&&!emissions&&td.sp_patch==pending&&saved[3]==pending);
  checks++;
 }
 /* All existing execution modes consume the same pending byte delta. */
 for(int tu=0;tu<2;tu++)for(int aot=0;aot<2;aot++)for(int direct=0;direct<2;direct++){
  setup(48);td.is_tu_mode=tu;option_sys_aot=option_aot_save=in_aot_translate=aot;hamt=direct;
  assert(latxs_ret_can_consume_sp(ir+3));
  assert(!latx_adjust_sp_before_translate(&tb,ir+3,3));assert(saved[3]==48);
  assert(latxs_translate_ret(ir+3));assert(load_offset==48&&emitted_delta==56&&td.sp_patch==0);checks++;
 }
 for(int pending=-2048;pending<=2039;pending+=17){
  if(!pending)continue;
  setup(pending);assert(!latx_adjust_sp_before_translate(&tb,ir+3,3));load_fault=true;
  if(!setjmp(fault)){latxs_translate_ret(ir+3);assert(0);}
  assert(event_count==1&&!emissions&&td.sp_patch==pending&&saved[3]==pending);
  checks++;
 }
 printf("PASS RET shared matcher/hook/emitter checks=%u\n",checks);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 for flags in [[],['-DTEST_WITH_TU=1']]:
  subprocess.run(['cc','-O2','-Wno-unused-function',*flags,str(p/'test.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
