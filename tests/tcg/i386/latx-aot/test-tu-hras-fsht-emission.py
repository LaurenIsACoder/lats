#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check actual CALL lookup emission against the shared cache-hit paths.

MMU/hash/HAMT helpers are IR markers here; native checks cover their real code.
The failed guard selects empty/flags/PC/invalid/CS-base NJC misses, followed by
an FSHT hit. This catches extra branches and wrong final CALL registers.
"""
from pathlib import Path
import itertools,os,re,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[4]
source=(root/'target/i386/latx/optimization/sys-intb-fsht.c').read_text()
ops=sorted(set(re.findall(r'\bLISA_[A-Z0-9_]+\b',source))|{'LISA_LABEL','LISA_BGE'})
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_SOFTMMU 1
#define FSHT_BITS 20
#define FSHT_SIZE (1ULL << (FSHT_BITS + 3))
#define g_assert assert
#define LOAD_HRAS_FSHT_BASE 1
#define HRAS_TU_CP_CHECK_PTR 2
#define add_curr_rel_entry(...) ((void)0)
#define tb_sc_table_offset_jmp(...) assert(!option_aot_save)
#define CONFIG_TB_FLAGS_NO_EFLAGS 1
#define HF_CS64_MASK 128
#define MMU_KNOSMAP_IDX 0
#define MMU_USER_IDX 2
#define R_CS 0
#define LATX_HELPER_LOOKUP_TB_HASHTABLE 1
#define latx_helper_lookup_tb_hashtable 0x200000
#define latx_helper_lookup_tb_hashtable_call 0x200100
#define ADDR uintptr_t
typedef struct { int val; } IR2_OPND;
typedef struct { struct {void *ptr;} tc; unsigned flags,cflags,cs_base,misc_flags; uint64_t pc,hva,page_addr_mix; } TranslationBlock;
typedef struct { unsigned hflags; uint64_t eip; struct { uint64_t base; } segs[1]; uint64_t tmparea[2]; } CPUX86State;
typedef struct { uint64_t tb;
#ifdef LATX_PCREL
 uint64_t pc;
#endif
} CPUJumpCacheEntry;
typedef struct { uint64_t tcptr,call_tcptr,pc; unsigned flags; uint16_t observed_call_entry,direct_entry_off; } LatxNJCEntry;
typedef struct { uint64_t base; unsigned flags; } flaged_shadow_hash_table_t;
static flaged_shadow_hash_table_t fsht[1];
static TranslationBlock block={.tc={(void*)0x100000}};
typedef struct {int mtcc_id,ir2_asm_nr; TranslationBlock *curr_tb; uint64_t tcg_region_offset[1];} TRANSLATION_DATA;
static TRANSLATION_DATA data={.curr_tb=&block};
static struct {typeof(data) *tr_data;} env={&data},*lsenv=&env;
static IR2_OPND la_a0={4},la_v0={4},la_a1={5},la_a6={10},la_ra={1},la_zero={0},la_env={23};
static struct {uint64_t scs_prologue_nofp,scs_prologue,scs_epilogue_nofp,scs_epilogue,tu_cp_check_ptr_mmu_user,cs_native_to_bt_ret_0;} sc={0x100800,0x100900,0x100a00,0x100b00,0x100c00,0x100d00};
#define GET_SC_TABLE(rid,name) sc.name
static uintptr_t shared_entry;
#define SET_SC_TABLE(rid,name,value) ((void)(rid),shared_entry=(value))
#define GET_SC_TABLE_OFFSET(rid,name,offset) (sc.name+(offset)[rid])
static int label,reg,compact,njc,njc_inline,hamt;
static int option_fast_itlb,option_tu_hras=2,option_table_addr,option_njc_layout;
static int option_aot_save,in_aot_translate;
#define AOT_BLOCK_OBSERVED_CALL_ENTRY 0x100
static bool intb_njc_enabled(void){return njc;}
static bool intb_njc_inline_enabled(void){return njc_inline;}
static bool latx_jmp_cache_compact_enabled(void){return compact;}
static bool hamt_enabled(void){return hamt;}
static int regs[]={8,9,11,20,21,22};
static IR2_OPND latxs_ra_alloc_itemp(void){for(int i=0;i<6;i++)if(!(reg&(1<<i))){reg|=1<<i;return (IR2_OPND){regs[i]};}assert(0);return (IR2_OPND){0};}
static void latxs_ra_free_temp(IR2_OPND *r){for(int i=0;i<6;i++)if(regs[i]==r->val)reg&=~(1<<i);}
#define la_tmp_free(r) latxs_ra_free_temp(&(r))
static IR2_OPND la_new_label(void){return (IR2_OPND){++label};}
static void rec(int op,long long a,long long b,long long c,long long d){printf("%d %lld %lld %lld %lld\n",op,a,b,c,d);data.ir2_asm_nr++;}
#define lisa_call LISA_JIRL
#define la_out1_(op,a) la_out1(op,a)
#define la_out1(op,a) rec(op,(a)->val,0,0,0)
#define la_out2(op,a,b) rec(op,(a)->val,(b)->val,0,0)
#define la_out3(op,a,b,c) rec(op,(a)->val,(b)->val,(c)->val,0)
#define la_out2_(op,a,b) la_out2(op,a,b)
#define la_out2i(op,a,b,c) rec(op,(a)->val,(b)->val,c,0)
#define la_out3i(op,a,b,c,d) rec(op,(a)->val,(b)->val,(c)->val,d)
#define la_out2ii(op,a,b,c,d) rec(op,(a)->val,(b)->val,c,d)
#define la_outa(op,a) rec(op,a,0,0,0)
#define la_label(a) rec(LISA_LABEL,(a).val,0,0,0)
#define la_label_name(a,n) la_label(a)
#define la_comment(s) ((void)0)
#define la_imm(a,b) do { rec(100,(a)->val,b,0,0); \
    if ((b)==fsht[0].base) rec(100,(a)->val,b,0,0); } while (0)
#define latxs_append_ir2_jmp_far(a,link) rec(link?LISA_BL:LISA_B,a,0,0,0)
#define latxs_append_ir2_opnd2_(op,a,b) la_out2(op,a,b)
static void latxs_out_jmp_cache_hash(void*a,void*b,void*c){la_out3(101,(IR2_OPND*)a,(IR2_OPND*)b,(IR2_OPND*)c);}
static int expected_shared;
static bool latxs_tr_gen_compact_njc_slot(void*a,void*b,void*c,bool d){(void)a;(void)b;(void)c;assert(d==(expected_shared?option_table_addr:option_njc_layout));return false;}
#define tr_lookup_code_fast_itlb(a,b,c,d) rec(102,(a)->val,(b)->val,c,(d).val)
#define tr_lookup_code_hva(a,b,c,d) rec(103,(a)->val,(b)->val,c,(d).val)
static void latxs_tr_gen_intb_sync_gpc(void){rec(104,0,0,0,0);}
static void latxs_tr_dmw_in_fix_mem(IR2_OPND*a){rec(105,a->val,0,0,0);}
static void latxs_tr_dmw_out_fix_mem(IR2_OPND*a){rec(106,a->val,0,0,0);}
static int lsenv_offset_of_tb_jmp_cache_ptr(void*p){(void)p;return 256;}
static int lsenv_offset_of_latx_njc_cache_ptr(void*p){(void)p;return 512;}
static void latxs_tr_init(void*p){(void)p;reg=label=data.ir2_asm_nr=0;}
static void latxs_tr_fini(void){}
static int latxs_tr_ir2_assemble(void*p){(void)p;return data.ir2_asm_nr;}
static void latxs_tr_gen_call_to_helper(ADDR address,int type){rec(107,address,type,0,0);}
'''
post=r'''
int main(int argc,char**argv){assert(argc==9 || argc==10);expected_shared=(atoi(argv[7])==0 || atoi(argv[7])==3);option_tu_hras=atoi(argv[7]);compact=atoi(argv[1]);njc=atoi(argv[2]);njc_inline=atoi(argv[3]);int fast=atoi(argv[4]);option_fast_itlb=atoi(argv[5]);hamt=atoi(argv[6]);fsht[0].base=0x300000;fsht[0].flags=HF_CS64_MASK|3;
 if(argc==10){
  if(!strncmp(argv[9],"0x",2)){
   option_table_addr=1;
   fsht[0].base=strtoull(argv[9],NULL,0);
  }else{
   option_aot_save=atoi(argv[9]);
  }
 }
 if(atoi(argv[7])==3){gen_latxs_intb_fsht_call_lookup_tucpfast((void*)0x100000,0);assert(!shared_entry);return 0;}
#ifdef NEW_GENERATOR
 if(atoi(argv[7])){
 latxs_tr_init(NULL);IR2_OPND miss=la_new_label(),call=la_new_label(),target={0};
 if(atoi(argv[8])>=2){
  IR2_OPND njc_miss=la_new_label(),saved[5];
  gen_latxs_intb_fsht_call_njc_save(0,&njc_miss,&call,&target,saved);
  la_label(call);
  la_out2i(LISA_JIRL,option_tu_hras==2?&la_ra:&la_zero,&la_a0,0);
  /* A later TU member can leave a completely different allocator state.
   * The cold fragment must still use the source NJC's physical registers. */
  reg=atoi(argv[8])==2?21:42;
  gen_latxs_intb_fsht_call_miss_saved(0,fast,&njc_miss,&miss,&call,saved);
  assert(reg==(atoi(argv[8])==2?21:42));reg=0;
 }else if(atoi(argv[8])){
  IR2_OPND njc_miss=la_new_label(),lookup=la_new_label(),irq={8};
  la_out3(LISA_BGE,&irq,&la_zero,&lookup);
  gen_latxs_intb_fsht_call_miss(0,fast,&njc_miss,&miss,&call);
  la_label(lookup);
  gen_latxs_intb_fsht_call_njc(0,&njc_miss,&call,&target);
 }else gen_latxs_intb_fsht_call(0,fast,&miss,&call,&target);
 assert(target.val==4 && reg==0 && !shared_entry);return 0;}

#endif
 if(fast)gen_latxs_intb_fsht_lookup_tucpfast((void*)0x100000,0);else gen_latxs_intb_fsht_lookup((void*)0x100000,0);
 if(compact)assert(shared_entry>0x100000 && !(shared_entry&3));
 return 0;}
'''
def walk(raw, failed_guard, fsht_fail=False):
    code=[tuple(map(int,line.split())) for line in raw.decode().splitlines()]
    op={name:i for i,name in enumerate(ops)}
    labels={r[1]:i for i,r in enumerate(code) if r[0]==op['LISA_LABEL']}
    ip=count=guard=0
    gpc_saved=restored=False
    while ip<len(code):
        ins=code[ip];opcode=ins[0]
        if opcode==op['LISA_LABEL']:ip+=1;continue
        count+=1
        if opcode==104: gpc_saved=True
        if opcode in (102,103) and fsht_fail:
            assert gpc_saved
            # The real address lookup uses a1 as scratch before branching.
            restored=False
            ip=labels[ins[4]]
            continue
        if opcode==op['LISA_LD_D'] and ins[1:4]==(5,23,8):
            restored=True
        if opcode==op['LISA_JIRL']:return count,ins
        if opcode in (op['LISA_BEQZ'],op['LISA_BNEZ'],op['LISA_BEQ'],op['LISA_BNE']):
            target=ins[2] if opcode in (op['LISA_BEQZ'],op['LISA_BNEZ']) else ins[3]
            if guard==failed_guard:
                ip=labels[target];guard+=1;continue
            guard+=1
        elif opcode==op['LISA_BGE']:
            # The shared entry has the same existing positive IRQ branch.
            ip=labels[ins[3]];continue
        elif opcode==op['LISA_B']:
            if ins[1] not in labels:
                assert fsht_fail and gpc_saved and restored, ('C fallback loses guest target',ins)
                return count,ins
            ip=labels[ins[1]];continue
        ip+=1
    raise AssertionError('no callee transfer')

with tempfile.TemporaryDirectory(prefix='tu-hras-fsht-emission-') as td:
    td=Path(td)
    source=re.sub(r'^#include[^\n]*\n','',source,flags=re.M)
    paths=c_misses=0
    for pcrel in (False,True):
        c=td/'emission.c';binary=td/'emission'
        c.write_text('enum {'+','.join(ops)+',lisa_mov};\n'+pre+source+post)
        subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=gnu11','-O0','-w',
                        *(['-DLATX_PCREL'] if pcrel else []),'-DNEW_GENERATOR',
                        str(c),'-o',str(binary)],check=True)
        # Only a shared stub can rely on the current table's alignment.
        for compact in ('0', '1'):
            for mode, base, insert in [('0', '0x800000', True), ('0', '0x801000', False),
                                       ('2', '0x800000', False)]:
                raw = subprocess.check_output([str(binary), compact, '1', '0', '0', '1', '1', mode, '0', base])
                code = [tuple(map(int, line.split())) for line in raw.splitlines()]
                slot_insert = any(op == ops.index('LISA_BSTRINS_D') and (hi, lo) == (22, 3)
                                  for op, _, _, hi, lo in code)
                assert slot_insert == insert, (compact, mode, base)
                if compact == '0':
                    base_load = next(i for i, ins in enumerate(code)
                                     if ins[0] == ops.index('LISA_LD_D') and ins[2:4] == (23, 256))
                    assert code[base_load + 1][0] == ops.index('LISA_ALSL_D')
                    # Inline legacy code must still accept an unwindowed base.
                    assert (code[base_load + 2][0] == 105) == (mode != '0')
        for compact,fast,itlb,hamt in itertools.product((0,1),repeat=4):
            args=[str(compact),'1','0',str(fast),str(itlb),str(hamt)]
            shared=subprocess.check_output([str(binary),*args,'0','0','0'])
            for mode in (1,2):
                for split in (0,1,2,3):
                    call=subprocess.check_output([str(binary),*args,str(mode),str(split),'0'])
                    if split==2: saved_call=call
                    if split==3: assert call==saved_call, 'Cold FSHT depends on unrelated allocator state'
                    for failure in [None,*range(3 if compact else 5)]:
                        old_count,_=walk(shared,failure)
                        new_count,last=walk(call,failure)
                        assert last[1:3]==(1 if mode==2 else 0,4),last
                        # Prefix includes the common IRQ BGE. Its FSHT hit
                        # also returns to CALL with B, reusing main's entry B.
                        extra = (0 if not split else
                                 (1 if failure is None else 2) if split==1 else
                                 (0 if failure is None else 1))
                        assert new_count==old_count+extra,(pcrel,compact,fast,itlb,hamt,mode,split,failure,old_count,new_count)
                        paths+=1
                    _,last=walk(call,0,fsht_fail=True)
                    assert last[0]==ops.index('LISA_B') and last[1]==1,last
                    c_misses+=1
        for split in (0,1):
            # The split NJC fragment receives cold_miss=NULL. Its observation
            # guard must use the defined NJC island, never a null/far label.
            call=subprocess.check_output([str(binary),'1','1','0','0','1','1','2',str(split),'1'])
            code=[tuple(map(int,line.split())) for row in call.decode().splitlines() if (line:=row)]
            labels={row[1] for row in code if row[0]==ops.index('LISA_LABEL')}
            for row in code:
                if row[0] in (ops.index('LISA_BEQZ'),ops.index('LISA_BNEZ')):
                    assert row[2] in labels, ('unresolved capture branch',row)
            _,last=walk(call,3)
            assert last[1:3]==(1,4),last
        # Verify compact njc_inline=1 behavior:
        # 1) ordinary jmp lookup (mode=0) skips NJC lookup and never stores to $ra (reg 1) on FSHT hit
        jmp_inline=subprocess.check_output([str(binary),'1','1','1','1','1','1','0','0','0'])
        jmp_code=[tuple(map(int,line.split())) for line in jmp_inline.decode().splitlines()]
        assert not any(r[0]==101 for r in jmp_code), 'jmp lookup with njc_inline=1 should not hash NJC'
        assert not any(r[0] in (ops.index('LISA_ST_W'),ops.index('LISA_ST_D')) and r[2]==1 for r in jmp_code), \
            'jmp lookup with njc_inline=1 must not store to uninitialized $ra'
        # 2) call_target lookup (mode=3) performs NJC lookup, initializes $ra, and stores to $ra on FSHT hit
        call_tucp=subprocess.check_output([str(binary),'1','1','1','1','1','1','3','0','0'])
        call_code=[tuple(map(int,line.split())) for line in call_tucp.decode().splitlines()]
        assert any(r[0]==101 for r in call_code), 'call_lookup_tucpfast with njc_inline=1 must hash NJC'
        assert any(r[0]==len(ops) and r[1]==1 for r in call_code), 'call_lookup_tucpfast must initialize $ra'
        assert any(r[0]==ops.index('LISA_ST_D') and r[2]==1 for r in call_code), \
            'call_lookup_tucpfast must populate NJC entry via $ra on FSHT hit'
print('PASS: %d FSHT address misses restore a1 before the C lookup'%c_misses)
print('PASS: %d NJC/FSHT hit paths retain checks without more emitted operations vs shared entry'%paths)
