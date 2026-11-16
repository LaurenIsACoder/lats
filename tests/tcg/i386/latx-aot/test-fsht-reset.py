#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise actual FSHT writers and resets, including lookup reinsertion."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[4]
def block(text, prefix):
    start=text.index(prefix);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
source=(root/'accel/tcg/translate-all.c').read_text()
header=(root/'target/i386/latx/include/latx-shadow-htable.h').read_text()
c=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#define CONFIG_SOFTMMU 1
#define FSHT_SIZE 128
#define FSHT_MASK 15
#define HF_CPL_MASK 3
#define CF_NO_GOTO_TB 2
#define CF_NO_GOTO_PTR 4
#define qatomic_read(p) __atomic_load_n(p,__ATOMIC_RELAXED)
#define qatomic_set(p,v) __atomic_store_n(p,v,__ATOMIC_RELAXED)
static uint64_t cmpxchg(uint64_t*p,uint64_t old,uint64_t value){__atomic_compare_exchange_n(p,&old,value,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);return old;}
#define qatomic_cmpxchg(p,o,v) cmpxchg((p),(o),(uintptr_t)(v))
typedef uint64_t vaddr;
typedef uint64_t tb_page_addr_t;
typedef struct {uint64_t pc,page_addr[2];uint32_t flags,cflags;} TranslationBlock;
static int64_t tb_page_addr1(TranslationBlock*t){return t->page_addr[1];}
static unsigned tb_cflags(TranslationBlock*t){return t->cflags;}
static unsigned option_arch_opt,option_flaged_sht=1,fsht_num=4;
static uint8_t fsht_flag_ids[32];
'''+block(header,'typedef struct flaged_shadow_hash_table')+' flaged_shadow_hash_table_t;\n'
c+='static flaged_shadow_hash_table_t fsht[4];\n'
for prefix in ['static inline unsigned fsht_flags_key(', 'static inline int fsht_flags_to_id(']:c+=block(header,prefix)+'\n'
c+=r'''
static unsigned cleared;
static void *clear_table(void*p,int value,size_t n){assert(!value&&n==FSHT_SIZE);cleared++;return memset(p,value,n);}
#undef memset
#define memset clear_table
'''
for prefix in ['static void flaged_shadow_htable_clear(', 'void flaged_shadow_htable_reset(', 'void flaged_shadow_htable_reset_mtcc(']:c+=block(source,prefix)+'\n'
c+='#undef memset\n'
for prefix in ['void flaged_shadow_htable_insert(', 'void flaged_shadow_htable_remove(']:c+=block(source,prefix)+'\n'
c+=block((root/'accel/tcg/cpu-exec.c').read_text(),'static inline void tb_htable_reinsert_fsht(')+'\n'
c+=r'''
static void *writer(void*p){TranslationBlock*t=p;tb_htable_reinsert_fsht(t,t->pc,t->page_addr[0],t->flags);return NULL;}
static void *publisher(void *p) {
    flaged_shadow_htable_insert(p);
    return NULL;
}
int main(void){
    unsigned flags[]={0x40c0b0,0x40c0b3,0x40c2b0,0x40c2b3};
    for(unsigned i=0;i<4;i++){fsht[i].flags=flags[i];fsht_flag_ids[fsht_flags_key(flags[i])]=i+1;fsht[i].base=(uintptr_t)calloc(1,FSHT_SIZE);assert(fsht[i].base);}
    for(option_arch_opt=0;option_arch_opt<2;option_arch_opt++){
        cleared=0;flaged_shadow_htable_reset();assert(cleared==(option_arch_opt?0:4));
        TranslationBlock a={.pc=1,.page_addr={1,-1},.flags=flags[0]},b={.pc=2,.page_addr={2,-1},.flags=flags[1]};
        flaged_shadow_htable_insert(&a);flaged_shadow_htable_insert(&b);
        flaged_shadow_htable_remove(&b);tb_htable_reinsert_fsht(&b,b.pc,2,b.flags);
        assert(((uint64_t*)fsht[0].base)[1]==(uintptr_t)&a);assert(((uint64_t*)fsht[1].base)[2]==(uintptr_t)&b);
        cleared=0;flaged_shadow_htable_reset_mtcc(0);assert(cleared==(option_arch_opt?1:2));
        assert(!((uint64_t*)fsht[0].base)[1]);assert(((uint64_t*)fsht[1].base)[2]==(uintptr_t)&b);
        TranslationBlock other=b;flaged_shadow_htable_remove(&other);assert(((uint64_t*)fsht[1].base)[2]==(uintptr_t)&b);
        flaged_shadow_htable_remove(&b);assert(!((uint64_t*)fsht[1].base)[2]);
        cleared=0;flaged_shadow_htable_reset();assert(cleared==(option_arch_opt?1:4));
        for(unsigned bad=0;bad<3;bad++){
            TranslationBlock t=a;if(bad==0)t.page_addr[1]=4096;else t.cflags=bad==1?CF_NO_GOTO_TB:CF_NO_GOTO_PTR;
            flaged_shadow_htable_insert(&t);tb_htable_reinsert_fsht(&t,t.pc,1,t.flags);assert(!((uint64_t*)fsht[0].base)[1]);
        }
        /* Publish from different pages into two slots of the same table. */
        TranslationBlock c = a;
        c.pc = c.page_addr[0] = 0x2003;
        pthread_t p, q;
        assert(!pthread_create(&p, NULL, publisher, &a));
        assert(!pthread_create(&q, NULL, publisher, &c));
        pthread_join(p, NULL);
        pthread_join(q, NULL);
        assert(((uint64_t *)fsht[0].base)[1] == (uintptr_t)&a);
        assert(((uint64_t *)fsht[0].base)[3] == (uintptr_t)&c);
        flaged_shadow_htable_remove(&a);flaged_shadow_htable_remove(&c);
        assert(!pthread_create(&p, NULL, writer, &a));
        assert(!pthread_create(&q, NULL, writer, &c));
        pthread_join(p, NULL);
        pthread_join(q, NULL);
        cleared=0;flaged_shadow_htable_reset();assert(cleared==(option_arch_opt?1:4));
        for(unsigned i=0;i<4;i++)for(unsigned j=0;j<16;j++)assert(!((uint64_t*)fsht[i].base)[j]);
        TranslationBlock dense[4][16]={0};
        for(unsigned i=0;i<4;i++)for(unsigned j=0;j<16;j++) {
            dense[i][j]=(TranslationBlock){.pc=j,.page_addr={j,-1},.flags=flags[i]};
            flaged_shadow_htable_insert(&dense[i][j]);
            assert(((uint64_t*)fsht[i].base)[j]==(uintptr_t)&dense[i][j]);
        }
        cleared=0;flaged_shadow_htable_reset();assert(cleared==4);
        for(unsigned i=0;i<4;i++)for(unsigned j=0;j<16;j++)assert(!((uint64_t*)fsht[i].base)[j]);
    }
    for(unsigned i=0;i<4;i++)free((void*)fsht[i].base);
    puts("PASS: clean/dirty global and MTCC reset, both writers, conditional removal, rejected insertions and concurrent publication/reinsertions");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(c)
    subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-sign-compare','-pthread',*shlex.split(os.environ.get('CFLAGS','')),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
