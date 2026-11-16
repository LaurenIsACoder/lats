#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Test the real runtime index builder and lookup, including wrapped collisions."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
source = (root / 'target/i386/latx/sbt/sys-aot-load.c').read_text()
internal = (root / 'target/i386/latx/sbt/sys-aot-internal.h').read_text()
header = (root / 'target/i386/latx/include/sys-aot-format.h').read_text()
if '#include "lats/aot-format.h"' in header:
    header = (root / 'include/lats/aot-format.h').read_text()
def block(text, prefix):
    start = text.index(prefix)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
c = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define DEBUG_AOT 0
#define ADDR uintptr_t
#define TARGET_PAGE_SIZE 4096
#define AOT_PAGE_MAGIC 0x66666666
#define ROUND_UP(n,a) (((n)+(a)-1)&~((a)-1))
typedef struct aot_tail aot_tail;
'''
for text, name in [(internal, 'AOTPageIndexEntry'), (header, 'page_head')]:
    c += block(text, 'typedef struct '+name+' {')+' '+name+';\n'
start=header.index('/* 页面信息：');end=header.index('static inline bool aot_page_info_size', start)
c += header[start:end]
c += block(source, 'typedef struct AOTLoadedFileIndex {')+' AOTLoadedFileIndex;\n'
c += r'''
static unsigned option_arch_opt=1;
static AOTPageIndexEntry *hash_table;
static uint32_t aot_hash_size;
static void *in_tb_ptr;
static bool aot_perf_enabled(void) { return false; }
static uint64_t aot_perf_now_ns(void) { return 0; }
static void aot_perf_record_lookup(bool a,uint32_t b,bool c,bool d,uint64_t e) {
    (void)a;(void)b;(void)c;(void)d;(void)e;assert(0);
}
static unsigned allocations;
static size_t allocated_bytes;
static void *allocate(size_t alignment,size_t bytes) {
    allocations++;allocated_bytes=bytes;
    assert(alignment==64 && bytes && bytes%64==0);
    return bytes>(1<<20)?NULL:aligned_alloc(alignment,bytes);
}
#define aligned_alloc allocate
'''
for signature in ['static bool aot_load_file_prepare_hash(',
                  'static void aot_load_file_build_hash(',
                  'static uint64_t find_page_with_hash(']:
    c += block(source, signature)+'\n'
c += r'''
#undef aligned_alloc
int main(void) {
    unsigned sizes[]={0,1,2,3,5,63,64,65,129};
    const size_t stride=sizeof(page_head)+TARGET_PAGE_SIZE;
    for(unsigned test=0;test<sizeof(sizes)/sizeof(sizes[0]);test++) {
        unsigned n=sizes[test];
        page_info_array_t *info=calloc(1,sizeof(*info)+n*sizeof(info->pages[0]));
        assert(info);info->page_count=n;
        AOTLoadedFileIndex index={.page_num=n,.info_array=info};
        assert(aot_load_file_prepare_hash(&index));
        size_t bytes=(n?n:1)*stride;
        uint8_t *file=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        assert(file!=MAP_FAILED);
        for(unsigned i=0;i<n;i++) {
            uint32_t hash=(i%3)*index.hash_size+index.hash_size-1;
            page_head *page=(page_head*)(file+i*stride);
            *page=(page_head){.page_magic=AOT_PAGE_MAGIC,.page_hash=hash};
            memset(page+1,i,TARGET_PAGE_SIZE);
            info->pages[i]=(page_info_t){.page_hash=hash,.file_offset=i*stride};
        }
        aot_load_file_build_hash(&index);
        hash_table=index.hash_table;aot_hash_size=index.hash_size;in_tb_ptr=file;
        uint8_t ram[TARGET_PAGE_SIZE]={0};
        for(unsigned enabled=0;enabled<2;enabled++) {
            option_arch_opt=enabled;
            for(unsigned i=0;i<n;i++) {
                memcpy(ram,file+i*stride+sizeof(page_head),sizeof(ram));
                assert(find_page_with_hash((uintptr_t)ram,info->pages[i].page_hash,0)==(uintptr_t)file+i*stride);
                ram[37]^=0xff;
                assert(!find_page_with_hash((uintptr_t)ram,info->pages[i].page_hash,0));
            }
        }
        /* A nonmatching cached hash must not read any mapped page header. */
        assert(!mprotect(file,bytes,PROT_NONE));
        uint32_t absent=(index.hash_size-1)|0x80000000u;
        assert(!find_page_with_hash((uintptr_t)ram,absent,0));
        assert(!munmap(file,bytes));
        free(info);free(hash_table);
    }
    AOTLoadedFileIndex huge={.page_num=UINT32_MAX};
    allocations=0;assert(!aot_load_file_prepare_hash(&huge));assert(!allocations);
    huge.page_num=(1u<<30)+1;assert(!aot_load_file_prepare_hash(&huge));assert(!allocations);
    huge.page_num=1u<<30;assert(!aot_load_file_prepare_hash(&huge));
    if(SIZE_MAX>UINT32_MAX)assert(allocations==1 && allocated_bytes==(UINT64_C(1)<<31)*sizeof(AOTPageIndexEntry));
    puts("PASS: index allocation bounds/alignment, wraparound, full-hash collisions, byte comparison and no page read on cached-hash mismatch");
}
'''
with tempfile.TemporaryDirectory(prefix='aot-page-index-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(c)
    subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-sign-compare',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
