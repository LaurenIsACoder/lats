#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Execute the actual leaf NJC emitter's IR for every hit/miss outcome.

Checks preservation of the hardware return link, guest registers, r2 and a6;
the translated SMC fixture separately exercises the real caller and runtime.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'target/i386/latx/optimization/sys-intb-njc.c').read_text()


def block(text, prefix):
    start = text.index(prefix)
    pos = text.index('{', start) + 1
    depth, end = 1, pos
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


c = r'''
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_SOFTMMU 1
#define CONFIG_TB_FLAGS_NO_EFLAGS 1
#define TARGET_PAGE_BITS 12
#define TB_JMP_CACHE_BITS 12
#define TB_JMP_PAGE_BITS 6
#define g_assert assert
#define g_assert_not_reached() assert(0)
#define G_STATIC_ASSERT(x) _Static_assert(x,#x)
typedef struct { bool used; unsigned physical_id; } TEMP_REG_STATUS;
typedef uint64_t vaddr;
'''
c += block((root / 'target/i386/latx/include/env.h').read_text(),
           'static const TEMP_REG_STATUS latxs_itemp_status_default[]') + ';\n'
c += block((root / 'accel/tcg/tb-jmp-cache.h').read_text(),
           'typedef struct LatxNJCEntry') + ' LatxNJCEntry;\n'
c += r'''
typedef struct { unsigned val; } IR2_OPND;
typedef struct { uint64_t cache; uint32_t hflags; } CPUX86State;
typedef enum { LATX_JMP_CACHE_HASH_PAGE, LATX_JMP_CACHE_HASH_FULL, LATX_JMP_CACHE_HASH_LINE } LatxJmpCacheHashMode;
static unsigned option_njc_layout, option_table_addr, option_njc_source_jump, bits, allocated;
static LatxJmpCacheHashMode hash_mode;
static void *lsenv;
static IR2_OPND latxs_env_ir2_opnd={23}, latxs_arg1_ir2_opnd={5},
    latxs_ret0_ir2_opnd={4}, latxs_zero_ir2_opnd={0},
    latxs_stmp1_ir2_opnd={8}, latxs_stmp2_ir2_opnd={9}, la_ra={1};
#define la_zero latxs_zero_ir2_opnd
#define la_a0 latxs_ret0_ir2_opnd
static unsigned latx_jmp_cache_bits(void) {return bits;}
static LatxJmpCacheHashMode latx_jmp_cache_hash_mode(void) {return hash_mode;}
static int lsenv_offset_of_latx_njc_cache_ptr(void *p) {(void)p;return offsetof(CPUX86State,cache);}
static int lsenv_offset_of_tb_jmp_cache_ptr(void *p) {(void)p;assert(0);return 0;}
static bool intb_njc_enabled(void) {return true;}
static bool hamt_enabled(void) {return true;}
static void latxs_tr_dmw_in_fix_mem(IR2_OPND *p) {(void)p;assert(0);}
enum { LISA_SRLI_D, LISA_SLLI_D, LISA_XOR, LISA_ADD_D, LISA_ADDI_D, LISA_BSTRPICK_D,
       LISA_BSTRINS_D, LISA_LD_D, LISA_LD_WU, LISA_BEQ, LISA_BNE,
       LISA_JIRL, LISA_ALSL_D, LISA_BEQZ, LISA_BL, LISA_B, LISA_PCADDU18I, LISA_INLINE, lisa_mov };
#define LISA_LOAD_PC LISA_LD_D
typedef struct { int op; unsigned d,a,b,hi,lo; } Instruction;
static Instruction code[128];
static unsigned nr, labels, label_pos[16];
static void out2i(int op, const IR2_OPND *d, const IR2_OPND *a, unsigned imm) {
    assert(nr<128);code[nr++]=(Instruction){op,d->val,a->val,imm,0,0};
}
static void out3(int op, const IR2_OPND *d, const IR2_OPND *a, const IR2_OPND *b) {
    assert(nr<128);code[nr++]=(Instruction){op,d->val,a->val,b->val,0,0};
}
static void out2ii(int op, const IR2_OPND *d, const IR2_OPND *a, unsigned hi,unsigned lo) {
    assert(nr<128);code[nr++]=(Instruction){op,d->val,a->val,0,hi,lo};
}
static void out2(int op,const IR2_OPND *d,const IR2_OPND *a) {out2i(op,d,a,0);}
static void latxs_append_ir2_opnd3i(int op,const IR2_OPND *d,const IR2_OPND *a,const IR2_OPND *b,unsigned n) {
    (void)op;(void)d;(void)a;(void)b;(void)n;assert(0); /* legacy-only address path */
}
#define latxs_append_ir2_opnd2i out2i
#define latxs_append_ir2_opnd3 out3
#define latxs_append_ir2_opnd2ii out2ii
#define latxs_append_ir2_opnd2_ out2
#define la_out2i out2i
#define la_out3 out3
#define la_out2ii out2ii
#define la_out2_ out2
#define la_out2 out2
static IR2_OPND la_new_label(void) {assert(labels<16);return (IR2_OPND){labels++};}
static void la_label(IR2_OPND label) {label_pos[label.val]=nr;}
static IR2_OPND latxs_ra_alloc_itemp(void) {
    for(unsigned i=0;i<6;i++)if(!(allocated&(1u<<i))) {
        allocated|=1u<<i;return (IR2_OPND){latxs_itemp_status_default[i].physical_id};
    }
    assert(0);return (IR2_OPND){0};
}
static void latxs_ra_free_temp(IR2_OPND *reg) {
    for(unsigned i=0;i<6;i++)if(latxs_itemp_status_default[i].physical_id==reg->val) {
        assert(allocated&(1u<<i));allocated&=~(1u<<i);return;
    }
    assert(0);
}
static void latxs_tr_init(void *p) {(void)p;nr=labels=allocated=0;memset(label_pos,0xff,sizeof(label_pos));}
static int latxs_tr_ir2_assemble(void *p) {(void)p;assert(!allocated);return nr;}
static void latxs_tr_fini(void) {assert(!allocated);}
'''
for prefix in ['void latxs_out_jmp_cache_hash(',
               'bool latxs_tr_gen_compact_njc_slot(',
               'static void latxs_tr_gen_intb_njc_slot(',
               'static void latxs_tr_gen_current_flags(',
               'void gen_latxs_intb_lookup_njc_compact(',
               'int gen_latxs_intb_njc_return(']:
    c += block(source, prefix) + '\n'
c += r'''
static void gen_inline_probe(void) {
    latxs_tr_init(NULL);
    IR2_OPND t=latxs_ra_alloc_itemp(),t0=latxs_ra_alloc_itemp(),t1=latxs_ra_alloc_itemp();
    IR2_OPND miss=la_new_label();
    gen_latxs_intb_lookup_njc_compact(NULL,&t,&t0,&t1,&miss,&la_ra,option_table_addr);
    la_out2i(LISA_JIRL,&la_zero,&la_ra,0);la_label(miss);
    la_out2i(LISA_JIRL,&la_zero,&la_ra,0);
    latxs_ra_free_temp(&t1);latxs_ra_free_temp(&t0);latxs_ra_free_temp(&t);
    latxs_tr_ir2_assemble(NULL);
    unsigned moves=0;for(unsigned i=0;i<nr;i++)moves+=code[i].op==lisa_mov;
    assert(moves==(option_table_addr?0:1));
}
enum { X86_INS_JMP, X86_INS_CALL, X86_INS_RET, HF_CS64_MASK=1<<15, HF_CPL_MASK=3 };
typedef struct { uint64_t pc,cs_base; unsigned flags; } TestTB;
typedef TestTB TranslationBlock;
typedef struct { unsigned ir2_asm_nr; uint64_t tcg_region_offset[4]; } TestTR;
typedef struct {
    unsigned branch_opcode,curr_mtcc_id;bool target_is_indirect;
    TestTB *curr_tb;TestTR *tr_ctx;
} ExitTBContext;
static int option_tu_hras,in_aot_translate,option_aot_save,option_aot_load,test_fid;
static bool jit_tu=true;
static bool jit_tu_requested(void){return jit_tu;}
static bool aot_tu_enabled(void){return true;}
#define INTB_NJC_RETURN 123
#define INTB_FSHT_AFTER_NJC 124
static int relocation_kind;
static void gen_latxs_intb_lookup_njc_inline(bool compact,bool aligned){assert(compact && aligned);code[nr++]=(Instruction){.op=LISA_INLINE};}
static unsigned far_call,after_call;
static void fake_call(void) {
    if(far_call)code[nr++]=(Instruction){.op=LISA_PCADDU18I};
    code[nr++]=(Instruction){.op=LISA_BL};after_call=nr;
}
#define GET_SC_TABLE(rid,name) ((void)(rid),UINT64_C(0x1000))
#define tb_sc_table_offset_jmp(tb,rid,name,region,size,link,kind) do { \
    (void)(size);relocation_kind=(kind); \
    if (link) fake_call(); else code[nr++]=(Instruction){.op=LISA_B}; \
} while(0)
'''
eob = (root / 'target/i386/latx/translator/sys-eob.c').read_text()
c += block(eob, 'typedef enum NJCJumpPath') + ' NJCJumpPath;\n'
c += block(eob, 'static NJCJumpPath latxs_tr_exit_tb_njc_path(') + '\n'
c += 'static void caller(const ExitTBContext *ctx) {\n    int offset=0,link=0,fid=test_fid;\n'
c += '    NJCJumpPath path = latxs_tr_exit_tb_njc_path(ctx, fid);\n'
c += block(eob, '    switch (path) {')
c += block(eob, '    if (path == NJC_JUMP_SOURCE_INLINE) {') + '\n}\n'
c += r'''
static void test_callers(void) {
    unsigned cases=0;
    for(unsigned mode=0;mode<=2;mode+=2)
    for(far_call=0;far_call<=1;far_call++)
    for(unsigned invalid=0;invalid<12;invalid++) {
        TestTB tb={.flags=HF_CS64_MASK|HF_CPL_MASK};TestTR tr={0};
        ExitTBContext ctx={.branch_opcode=X86_INS_JMP,.target_is_indirect=true,.curr_tb=&tb,.tr_ctx=&tr};
        option_njc_source_jump=mode;option_tu_hras=2;in_aot_translate=option_aot_save=0;test_fid=0;
        if(invalid==1)ctx.branch_opcode=X86_INS_CALL;
        if(invalid==2)ctx.branch_opcode=X86_INS_RET;
        if(invalid==3)ctx.target_is_indirect=false;
        if(invalid==4)tb.flags&=~HF_CS64_MASK;
        if(invalid>=5 && invalid<=7)tb.flags=(tb.flags&~HF_CPL_MASK)+(invalid-5);
        if(invalid==8)tb.cs_base=1;
        if(invalid==9)test_fid=-1;
        if(invalid==10)in_aot_translate=1;
        if(invalid==11)option_tu_hras=1;
        latxs_tr_init(NULL);caller(&ctx);
        if(!mode || invalid)assert(nr==0);
        else {
            assert(after_call==1+far_call && labels==1 && label_pos[0]==nr);
            unsigned jump=after_call;
            assert(code[jump].op==LISA_JIRL && code[jump].d==0 && code[jump].a==4);
            assert(nr==jump+1 && relocation_kind==INTB_NJC_RETURN);
            /* A miss-return +4 must reach the fallback, not a padding
             * instruction or the target JIRL, for both call encodings. */
            if(mode==2)assert(after_call+1==label_pos[0]);
        }
        cases++;
    }
    for (unsigned active=0;active<2;active++) for (unsigned aligned=0;aligned<2;aligned++) {
        TestTB tb={.flags=HF_CS64_MASK|HF_CPL_MASK};TestTR tr={0};
        ExitTBContext ctx={.branch_opcode=X86_INS_JMP,.target_is_indirect=true,.curr_tb=&tb,.tr_ctx=&tr};
        option_njc_source_jump=2;option_tu_hras=0;in_aot_translate=1;option_aot_save=active;
        option_table_addr=aligned;test_fid=0;far_call=0;
        latxs_tr_init(NULL);caller(&ctx);
        assert(nr==(active?2:0));
        if(active && aligned)assert(code[0].op==LISA_INLINE &&
            code[1].op==LISA_B && relocation_kind==INTB_FSHT_AFTER_NJC);
        if(active && !aligned)assert(relocation_kind==INTB_NJC_RETURN && code[1].op==LISA_JIRL);
        cases++;
    }
    for (unsigned aligned=0;aligned<2;aligned++) {
        TestTB tb={.flags=HF_CS64_MASK|HF_CPL_MASK};TestTR tr={0};
        ExitTBContext ctx={.branch_opcode=X86_INS_JMP,.target_is_indirect=true,.curr_tb=&tb,.tr_ctx=&tr};
        option_njc_source_jump=2;option_tu_hras=0;jit_tu=false;
        in_aot_translate=option_aot_save=option_aot_load=0;
        option_table_addr=aligned;test_fid=0;
        latxs_tr_init(NULL);caller(&ctx);
        assert(nr==(aligned?2:0));
        if(aligned)assert(code[0].op==LISA_INLINE && code[1].op==LISA_B && relocation_kind==INTB_FSHT_AFTER_NJC);
        cases++;
    }
    jit_tu=true;
    printf("PASS: %u actual caller-fragment cases; eligibility and mode-2 one-instruction skip contract\n",cases);
}
'''
c += r'''
static uint64_t mask(unsigned width) {assert(width && width<64);return (UINT64_C(1)<<width)-1;}
static unsigned exit_pc;
static unsigned execute(uint64_t *r) {
    for(unsigned pc=0,steps=0;pc<nr && steps<128;steps++) {
        Instruction i=code[pc++];
        uint64_t a=r[i.a], address;
        switch(i.op) {
        case LISA_SRLI_D:r[i.d]=a>>i.b;break;
        case LISA_SLLI_D:r[i.d]=a<<i.b;break;
        case LISA_XOR:r[i.d]=a^r[i.b];break;
        case LISA_ADD_D:r[i.d]=a+r[i.b];break;
        case LISA_ADDI_D:r[i.d]=a+i.b;break;
        case lisa_mov:r[i.d]=a;break;
        case LISA_BSTRPICK_D:r[i.d]=(a>>i.lo)&mask(i.hi-i.lo+1);break;
        case LISA_BSTRINS_D: {
            uint64_t m=mask(i.hi-i.lo+1);
            r[i.d]=(r[i.d]&~(m<<i.lo))|((a&m)<<i.lo);break;
        }
        case LISA_LD_D:case LISA_LD_WU:
            address=a+i.b;
            if(address>>48==0x9000)address&=UINT64_C(0x0000ffffffffffff);
            if(i.op==LISA_LD_D)memcpy(&r[i.d],(void *)(uintptr_t)address,8);
            else {uint32_t value;memcpy(&value,(void *)(uintptr_t)address,4);r[i.d]=value;}
            break;
        case LISA_BEQ:case LISA_BNE:
            if((r[i.d]==a)==(i.op==LISA_BEQ))pc=label_pos[i.b];
            break;
        case LISA_JIRL:
            assert(i.d==0 && i.a==1 && !i.b);
            exit_pc=pc-1;return steps+1;
        default:assert(0);
        }
        assert(!r[0]);
    }
    assert(0);return 0;
}
int main(void) {
    test_callers();
    unsigned cases=0;
    for(hash_mode=0;hash_mode<=LATX_JMP_CACHE_HASH_LINE;hash_mode++)
    for(bits=8;bits<=16;bits+=4) {
        if(hash_mode==LATX_JMP_CACHE_HASH_PAGE && bits!=12)continue;
        size_t entries=(size_t)1<<bits,bytes=entries*sizeof(LatxNJCEntry);
        void *allocation;assert(!posix_memalign(&allocation,bytes,bytes+64));
        for(option_njc_layout=0;option_njc_layout<=1;option_njc_layout++)
        for(option_njc_source_jump=2;option_njc_source_jump<=2;option_njc_source_jump++)
        for(unsigned direct=0;direct<2;direct++) {
            option_table_addr=option_njc_layout;
            LatxNJCEntry *cache=(void *)((char *)allocation+(option_njc_layout?0:16));
            if(direct)gen_inline_probe();else gen_latxs_intb_njc_return(NULL);
            for(unsigned window=0;window<2;window++)
            for(unsigned trial=0;trial<128;trial++)
            for(unsigned outcome=0;outcome<4;outcome++) {
                memset(cache,0,bytes);
                uint64_t guest_pc=UINT64_C(0x7fff89340000)+trial*4097;
                uint64_t t=guest_pc^(guest_pc>>6);
                unsigned h=hash_mode==LATX_JMP_CACHE_HASH_PAGE?((t>>6)&0xfc0)|(t&63):
                    hash_mode==LATX_JMP_CACHE_HASH_FULL?(guest_pc^(guest_pc>>bits))&(entries-1):guest_pc&(entries-1);
                uint64_t target=UINT64_C(0x90007ffe12345678);
                CPUX86State env={.cache=(uintptr_t)cache|(window?UINT64_C(0x9000000000000000):0),.hflags=0x10003};
                cache[h].tcptr=outcome? (void *)(uintptr_t)target:NULL;
                cache[h].pc=guest_pc+(outcome==1);
                cache[h].flags=env.hflags+(outcome==2);
                uint64_t r[32],before[32];
                for(unsigned i=0;i<32;i++)r[i]=UINT64_C(0x1357900000000000)+i;
                r[0]=0;r[5]=guest_pc;r[23]=(uintptr_t)&env;memcpy(before,r,sizeof(r));
                execute(r);
                if(outcome==3)assert(r[4]==target);
                if(direct) {
                    assert(r[1]==env.cache+h*sizeof(*cache));
                    assert((exit_pc==label_pos[0])==(outcome!=3));
                } else assert(r[1]==before[1]+(option_njc_source_jump==2 && outcome!=3?4:0));
                for(unsigned i=0;i<32;i++) {
                    bool temporary=i==4 || i==1;
                    for(unsigned j=0;j<6;j++)temporary|=latxs_itemp_status_default[j].physical_id==i;
                    if(!temporary)assert(r[i]==before[i]);
                }
                assert(cache[h].pc==guest_pc+(outcome==1) && cache[h].flags==env.hflags+(outcome==2));
                cases++;
            }
        }
        free(allocation);
    }
    printf("PASS: %u real-emitter leaf/inline outcomes; GPC/previous-TB/guest registers/r2/sp preserved; RA unchanged on hits, +4 only on misses\n",cases);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(c)
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
    command += shlex.split(os.environ.get('CFLAGS', '')) + [str(path / 'test.c'), '-o', str(path / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test')], check=True)
