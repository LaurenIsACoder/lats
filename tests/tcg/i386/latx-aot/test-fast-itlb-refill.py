#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise the real fast-iTLB refill emitter against the packed-page contract."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[4]
s = (root / 'target/i386/latx/translator/sys-softmmu.c').read_text()
start = s.index('void tr_refill_code_fast_itlb(')
end = s.index('{', start) + 1
depth = 1
while depth:
    depth += (s[end] == '{') - (s[end] == '}')
    end += 1
c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { unsigned val; } IR2_OPND;
typedef struct { uint64_t itlb; } CPUTLBDescFast;
typedef struct { struct { struct { CPUTLBDescFast f[3]; } tlb; } neg; uint64_t env; } ArchCPU;
#define TLB_MASK_TABLE_OFS(IDX) ((int)offsetof(ArchCPU, neg.tlb.f[IDX])-(int)offsetof(ArchCPU, env))
enum {LISA_LD_D,LISA_ADD_D,LISA_BSTRPICK_D,LISA_ALSL_D,LISA_SLLI_D,LISA_BSTRINS_D,LISA_OR,LISA_ST_D};
static IR2_OPND la_env={23};static uint64_t reg[32],stored,stored_addr;static int nr,option_table_addr,hamt;
static bool hamt_enabled(void){return hamt;}
static bool int32_in_int12(int n){return n>=-2048 && n<=2047;}
#define g_assert_not_reached() abort()
static void la_imm(IR2_OPND *dst,int64_t v){reg[dst->val]=v;nr++;}
static void latxs_tr_dmw_in_fix_mem(IR2_OPND *p){(void)p;nr++;}
static void la_out2i(int op,IR2_OPND *dst,IR2_OPND *src,int val){
 uint64_t v=reg[src->val];nr++;
 if(op==LISA_LD_D)reg[dst->val]=UINT64_C(0x10000000);
 else if(op==LISA_SLLI_D)reg[dst->val]=v<<val;
 else if(op==LISA_ST_D){stored=reg[dst->val];stored_addr=v+val;}
 else abort();
}
static void la_out2ii(int op,IR2_OPND *dst,IR2_OPND *src,int hi,int lo){
 uint64_t mask=(UINT64_C(1)<<(hi-lo+1))-1,v=reg[src->val];nr++;
 if(op==LISA_BSTRPICK_D)reg[dst->val]=(v>>lo)&mask;
 else if(op==LISA_BSTRINS_D)reg[dst->val]=(reg[dst->val]&~(mask<<lo))|((v&mask)<<lo);
 else abort();
}
static void la_out3(int op,IR2_OPND *dst,IR2_OPND *a,IR2_OPND *b){
 uint64_t x=reg[a->val],y=reg[b->val];nr++;
 if(op==LISA_OR)reg[dst->val]=x|y;else if(op==LISA_ADD_D)reg[dst->val]=x+y;else abort();
}
static void la_out3i(int op,IR2_OPND *dst,IR2_OPND *a,IR2_OPND *b,int sh){
 assert(op==LISA_ALSL_D && sh==2);nr++;reg[dst->val]=(reg[a->val]<<(sh+1))+reg[b->val];
}
''' + s[start:end] + r'''
static uint64_t state=UINT64_C(0xc3ed9214a780d1);
static uint64_t random64(void){state^=state<<13;state^=state>>7;state^=state<<17;return state;}
int main(void){
 IR2_OPND gva={4},gp={5},t0={6},t1={7},t2={8};unsigned long cases=0;
 for(int ps=2;ps<=4;ps+=2)for(int h=0;h<2;h++)for(int i=0;i<300000;i++){
  uint64_t va=i==0?UINT64_MAX:i==1?0:random64(),pte=i==2?UINT64_MAX:random64();
  uint64_t expected_gp=ps==4?(pte>>12)&((1ULL<<28)-1):
      (((pte>>21)&((1ULL<<19)-1))<<9)|((va>>12)&511);
  uint64_t expected=((va>>12)&((1ULL<<36)-1))<<28 | expected_gp;
  int counts[2];
  for(int mode=0;mode<=1;mode++){
   for(int j=0;j<32;j++)reg[j]=UINT64_C(0xa5a5a5a5a5a5a5a5);
   reg[gva.val]=va;reg[gp.val]=pte;stored=stored_addr=0;nr=0;hamt=h;option_table_addr=mode;
   tr_refill_code_fast_itlb(&gva,&gp,&t0,&t1,&t2,0,ps);
   assert(stored==expected && reg[gp.val]==expected_gp && reg[gva.val]==va);
   assert(stored_addr==UINT64_C(0x10000000)+(((va>>12)&1023)<<3));counts[mode]=nr;cases++;
  }
  assert(counts[0]-counts[1]==h);
 }
 printf("PASS: %lu actual-emitter cases, 4K/2M values and returned GPA; HAMT reduces one instruction\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='fast-itlb-refill-') as d:
    p=Path(d);(p/'test.c').write_text(c)
    subprocess.run(shlex.split(os.environ.get('CC','cc'))+['-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
