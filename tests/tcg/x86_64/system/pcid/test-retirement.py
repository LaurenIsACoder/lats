#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise PCID bank allocation and CPU-removal interleavings."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[5]


def function(path, name):
    source = (root / path).read_text()
    start = source.index(name)
    return source[start:source.index('\n}', start) + 2]


# Pause jump invalidation between dispatch and bank iteration. The reader's
# RCU section keeps the old allocation alive while teardown unpublishes it.
source = r'''
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_SOFTMMU 1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define qatomic_rcu_read(p) (*(p))
#define qatomic_set(p, v) (*(p) = (v))
#define qatomic_cmpxchg(p, old, newval) ({ \
    __typeof__(*(p)) _old = *(p);          \
    if (_old == (old)) *(p) = (newval);    \
    _old;                                  \
})
typedef uintptr_t vaddr;
typedef int TranslationBlock;
typedef struct { struct { TranslationBlock *tb; } array[1]; } CPUJumpCache;
typedef struct {
    struct {
        vaddr pc;
        const void *tcptr;
        uint16_t direct_entry_off;
        const void *call_tcptr;
    } array[1];
} LatxNJC;
typedef struct {
    CPUJumpCache *jc;
    LatxNJC *njc;
    void *user_itlb, *kernel_itlb;
    bool initialized;
    _Alignas(4 * sizeof(void *)) char fast_desc;
} LatxPcidBank;
struct LatxPcidCaches { LatxPcidBank bank[2]; int epoch; void *retired_cpu; };
typedef struct {
    struct LatxPcidCaches *latx_pcid_caches;
    CPUJumpCache *tb_jmp_cache;
    LatxNJC *latx_njc_cache;
    int itlb_knosmap, itlb_user;
} CPUState;
#define cpu_neg(cpu) (cpu)
#define qatomic_rcu_set(p, v) (*(p) = (v))
#define object_unref(p) ((void)(p))
#define g_new0(type, count) calloc(count, sizeof(type))
#define g_free free
static void *aligned;
static void *qemu_memalign(size_t alignment, size_t size)
{
    assert(alignment >= _Alignof(struct LatxPcidCaches));
    assert(posix_memalign(&aligned, alignment, size) == 0);
    return memset(aligned, 0xa5, size);
}
static void qemu_vfree(void *ptr)
{
    assert(ptr == aligned);
    free(ptr);
    aligned = NULL;
}
static void pcid_publish_roots(CPUState *cpu, LatxPcidBank *bank)
{
    assert(bank[0].initialized && !bank[1].initialized && !bank[1].jc);
}
static int option_pcid_cache = 1;
static unsigned tb_jmp_cache_hash(vaddr pc) { return 0; }
static const void *latx_njc_tcptr(TranslationBlock *tb) { return tb; }
static void tb_jmp_cache_invalidate_current(CPUState *c, vaddr p,
                                            TranslationBlock *tb) { abort(); }
'''
source += function('accel/tcg/cputlb.c', 'void latx_pcid_cache_init(')
source += function('accel/tcg/cputlb.c', 'static void pcid_cache_free(')
source += function('accel/tcg/cputlb.c', 'void latx_pcid_jump_invalidate(')
source += r'''
static int detach;
#define latx_pcid_jump_invalidate(cpu, pc, tb) do { \
    if (detach) (cpu)->latx_pcid_caches = NULL;     \
    latx_pcid_jump_invalidate(cpu, pc, tb);         \
} while (0)
'''
source += function('accel/tcg/tb-hash.h',
                   'static inline void tb_jmp_cache_invalidate(')
# Schedule removal after a CPU-list reader has acquired a CPU and before its
# first dirty-table access. A trylock makes the interleaving deterministic.
source += r'''
typedef uintptr_t ram_addr_t;
typedef char RAMBlock;
static RAMBlock test_block;
static CPUState test_cpu;
static pthread_mutex_t qemu_cpu_list_lock = PTHREAD_MUTEX_INITIALIZER;
static int removed, scanned;
#define qemu_mutex_lock_1 pthread_mutex_lock
#define qemu_mutex_unlock_1 pthread_mutex_unlock
#define TARGET_PAGE_ALIGN(x) (x)
#define TARGET_PAGE_MASK (~(ram_addr_t)0)
#define RCU_READ_LOCK_GUARD() ((void)0)
#define tcg_enabled() 1
#define qemu_get_ram_block(x) (&test_block)
#define ramblock_ptr(b, off) (b)
#define CPU_FOREACH(iter) for ((iter) = &test_cpu; (iter); (iter) = NULL)
static void *try_remove(void *unused)
{
    int rc = pthread_mutex_trylock(&qemu_cpu_list_lock);
    if (rc == 0) {
        removed = 1;
        pthread_mutex_unlock(&qemu_cpu_list_lock);
    } else {
        assert(rc == EBUSY);
    }
    return NULL;
}
static void tlb_reset_dirty(CPUState *c, ram_addr_t start, ram_addr_t len)
{
    pthread_t writer;
    assert(!pthread_create(&writer, NULL, try_remove, NULL));
    assert(!pthread_join(writer, NULL));
    assert(!removed);
    scanned++;
}
'''
source += function('softmmu/physmem.c',
                   'void tlb_reset_dirty_range_all(ram_addr_t start, ram_addr_t length)\n{')
source += r'''
int main(int argc, char **argv)
{
    if (!strcmp(argv[1], "allocation")) {
        latx_pcid_cache_init(&test_cpu);
        assert(aligned && test_cpu.latx_pcid_caches == aligned);
        pcid_cache_free(test_cpu.latx_pcid_caches);
        assert(!aligned);
    } else if (!strcmp(argv[1], "dirty")) {
        tlb_reset_dirty_range_all(0, 1);
        assert(scanned == 1);
        try_remove(NULL);
        assert(removed);
    } else {
        TranslationBlock tb;
        CPUJumpCache jc = { .array = {{ &tb }} };
        LatxNJC njc = { .array = {{ 1, &tb, 32, &tb }} };
        struct LatxPcidCaches cache = { .bank = {{0}, { &jc, &njc }} };
        test_cpu.latx_pcid_caches = &cache;
        tb_jmp_cache_invalidate(&test_cpu, 1, &tb);
        assert(!jc.array[0].tb && !njc.array[0].tcptr &&
               !njc.array[0].call_tcptr && !njc.array[0].direct_entry_off);
        detach = 1;
        tb_jmp_cache_invalidate(&test_cpu, 1, &tb);
    }
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'reader.c').write_text(source)
    exe = path / 'reader'
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11',
                    '-pthread', str(path / 'reader.c'), '-o', str(exe)], check=True)
    failed = []
    for case in ('allocation', 'jump', 'dirty'):
        result = subprocess.run([str(exe), case])
        if result.returncode:
            failed.append(case)
        print(f'{case}: {result.returncode}')
    if failed:
        raise SystemExit('Failed retirement cases: ' + ', '.join(failed))
