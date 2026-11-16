#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Each live context must own a distinct region after a full code flush."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "tcg/region.c").read_text()
begin = source.index("void tcg_region_reset_all(void)")
function = source[begin:source.index("\n}", begin) + 2]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define MTCC_NR 2
#define qatomic_read(p) (*(p))
#define g_assert(v) assert(v)
#define CONFIG_LATX_TU 1
#define CONFIG_SOFTMMU 1
typedef struct { unsigned mtcc_id; size_t assigned; bool skip_static; } TCGContext;
static struct { size_t current,current_mtcc[2],agg_size_full,mtcc_n,n_mtcc; int lock; } region;
static TCGContext contexts[8], *tcg_ctxs[8];
static unsigned tcg_cur_ctxs;
static bool mtcc,option_split_tb=true,option_split_search=true;
static bool used[16];
static inline bool qemu_tcg_mtcc_enabled(void) { return mtcc; }
static void qemu_mutex_lock(int *lock) { (void)lock; }
static void qemu_mutex_unlock(int *lock) { (void)lock; }
static void latxs_external_tu_stub_reset(unsigned id) { assert(id<2); }
static void region_reset_tb(unsigned id) { assert(id<2); }
static void region_reset_search(unsigned id) { assert(id<2); }
static void tcg_region_assign(TCGContext *s,size_t index,int skip) {
    assert(index<16 && !used[index]); used[index]=true;
    s->assigned=index; s->skip_static=skip;
}
static void tcg_region_initial_alloc__locked(TCGContext *s) {
    size_t index;
    if (mtcc) {
        unsigned id=s->mtcc_id;
        assert(region.current_mtcc[id]<region.mtcc_n);
        index=id*region.mtcc_n+region.current_mtcc[id]++;
    } else index=region.current++;
    tcg_region_assign(s,index,0);
}
'''
checks = r'''
int main(void) {
    for (int enabled=0;enabled<=1;enabled++) {
        for (unsigned count=2;count<=8;count+=2) {
            for (unsigned first=0;first<2;first++) {
                mtcc=enabled;tcg_cur_ctxs=count;
                memset(&region,0,sizeof(region));region.mtcc_n=8;region.n_mtcc=2;
                memset(used,0,sizeof(used));
                for (unsigned i=0;i<count;i++) {
                    contexts[i].mtcc_id=(i+first)%2;tcg_ctxs[i]=&contexts[i];
                }
                tcg_region_reset_all();
                bool seen[2]={false,false};
                for (unsigned i=0;i<count;i++) {
                    unsigned id=contexts[i].mtcc_id;
                    if (enabled) {
                        assert(contexts[i].assigned/8==id);
                        assert(contexts[i].skip_static==!seen[id]);seen[id]=true;
                    } else {
                        assert(contexts[i].assigned==i);
                        assert(contexts[i].skip_static==(i==0));
                    }
                }
            }
        }
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / "check.c"
    path.write_text(prefix + function + checks)
    binary = Path(directory) / "check"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: full reset gives each context a distinct region and preserves static prefixes")
