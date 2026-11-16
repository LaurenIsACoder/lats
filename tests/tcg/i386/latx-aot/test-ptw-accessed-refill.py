#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the production refill emitter's cold Accessed updates.

Interpret its emitted operations to schedule changes before CAS or LL and SC.
On LoongArch also encode and execute the same operations as native code.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'target/i386/latx/optimization/sys-hamt-excp.c').read_text()
begin = source.index('typedef struct HamtPtwAccessed {')
end = source.index('/* t0 holds the guest VA;', begin)
helpers = source[begin:end]

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#define g_assert assert
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PG_ACCESSED_MASK 0x20
#define PG_DIRTY_MASK 0x40
#define PG_PSE_MASK 0x80
#define CPUINFO_LAMCAS 1
static unsigned cpuinfo;
typedef struct { unsigned reg; } IR2_OPND;
enum { LISA_ANDI, LISA_ORI, LISA_ADD_D, LISA_SUB_D, LISA_LL_D,
       LISA_SC_D, LISA_DBAR, LISA_BEQZ, LISA_BNEZ, LISA_BNE,
       LISA_B, LISA_MASKEQZ, LISA_AMCAS_DB_D, LABEL, RETURN };
typedef struct { unsigned op, a, b, c; } Insn;
static Insn code[256];
static unsigned count, next_label = 32, labels[128], starts[4];
static IR2_OPND la_t1 = {13}, la_t2 = {14}, la_t3 = {15}, la_t4 = {16};
static IR2_OPND la_t6 = {18}, la_t7 = {19};
static IR2_OPND la_new_label(void) { return (IR2_OPND){next_label++}; }
static void emit(unsigned op, unsigned a, unsigned b, unsigned c) {
    assert(count < ARRAY_SIZE(code)); code[count++] = (Insn){op,a,b,c};
}
static void la_out2i(unsigned op, IR2_OPND *a, IR2_OPND *b, unsigned c) {
    emit(op, a->reg, b->reg, c);
}
static void la_out3(unsigned op, IR2_OPND *a, IR2_OPND *b, IR2_OPND *c) {
    emit(op, a->reg, b->reg, c->reg);
}
static void la_out2(unsigned op, IR2_OPND *a, IR2_OPND *b) {
    emit(op, a->reg, b->reg, 0);
}
static void la_out1(unsigned op, IR2_OPND *a) { emit(op, a->reg, 0, 0); }
static void la_outi(unsigned op, unsigned i) { emit(op, i, 0, 0); }
static void la_label(IR2_OPND label) { emit(LABEL, label.reg, 0, 0); }
'''
program += helpers
program += r'''
static unsigned checks;
static bool parent_readonly, parent_missing;
static void check(unsigned site, const char *name, uint64_t snapshot,
                  unsigned race, uint64_t replacement, bool succeeds)
{
    uint64_t regs[32] = {0}, saved[32], memory = snapshot;
    const uint64_t gpa = 0x7020, base = 0x9000000000;
    regs[14] = gpa; regs[15] = snapshot; regs[19] = base;
    for (unsigned i = 0; i < 32; i++) {
        if (i != 14 && i != 15 && i != 16 && i != 19) regs[i] = i + 100;
    }
    regs[18] = (parent_readonly ? 0 : 2) | (parent_missing ? 0 : PG_ACCESSED_MASK);
    if (race == 4) memory = replacement;
    memcpy(saved, regs, sizeof(regs));
    bool reserved = false, returned = false, result = false;
    unsigned steps = 0, writes = 0;
    for (unsigned pc = starts[site]; !returned; pc++) {
        assert(pc < count && ++steps < 100);
        Insn x = code[pc];
        switch (x.op) {
        case LABEL: break;
        case LISA_ANDI: regs[x.a] = regs[x.b] & x.c; break;
        case LISA_ORI: regs[x.a] = regs[x.b] | x.c; break;
        case LISA_ADD_D: regs[x.a] = regs[x.b] + regs[x.c]; break;
        case LISA_SUB_D: regs[x.a] = regs[x.b] - regs[x.c]; break;
        case LISA_DBAR: assert(x.a == 0); break;
        case LISA_MASKEQZ: regs[x.a] = regs[x.c] ? regs[x.b] : 0; break;
        case LISA_LL_D:
            assert(regs[x.b] + x.c == base + gpa);
            if (race == 1) memory = replacement;
            regs[x.a] = memory; reserved = true;
            break;
        case LISA_AMCAS_DB_D: {
            assert(regs[x.b] == base + gpa);
            if (race == 1 || race == 2) memory = replacement;
            uint64_t old = memory;
            if (old == regs[x.a]) { memory = regs[x.c]; writes++; }
            regs[x.a] = old;
            break;
        }
        case LISA_SC_D:
            assert(regs[x.b] + x.c == base + gpa);
            if (race == 2) { memory = replacement; reserved = false; }
            if (race == 3) reserved = false;
            if (reserved) { memory = regs[x.a]; writes++; }
            regs[x.a] = reserved;
            break;
        case LISA_BEQZ:
            if (!regs[x.a]) pc = labels[x.b];
            break;
        case LISA_BNEZ:
            if (regs[x.a]) pc = labels[x.b];
            break;
        case LISA_BNE:
            if (regs[x.a] != regs[x.b]) pc = labels[x.c];
            break;
        case LISA_B: pc = labels[x.a]; break;
        case RETURN: result = x.a; returned = true; break;
        default: assert(false);
        }
    }
    assert(result == succeeds);
    if (succeeds) {
        assert(memory == (race == 4 ? replacement : snapshot | PG_ACCESSED_MASK));
        assert(regs[14] == gpa && regs[15] == (snapshot | PG_ACCESSED_MASK));
        assert(writes == !(snapshot & PG_ACCESSED_MASK));
        if (snapshot & PG_ACCESSED_MASK) assert(steps == 9);
        assert(regs[18] == ((snapshot & PG_DIRTY_MASK) ? saved[18] & 2 : 0));
    } else {
        assert(!writes);
        assert(memory == ((race == 1 || race == 2) ? replacement : snapshot));
    }
    for (unsigned i = 0; i < 32; i++) {
        if ((i != 13 || !cpuinfo) && i != 14 && i != 15 && i != 16 && i != 18) assert(regs[i] == saved[i]);
    }
    checks++;
    printf("site%u %s PASS\n", site, name);
}

#ifdef __loongarch__
#include <pthread.h>
static bool native_stress;
typedef struct {
    uint64_t *entry;
    uint64_t snapshot;
    pthread_barrier_t barrier;
} Race;
static uint64_t replacement_for(Race *race, unsigned i) {
    switch (i % 4) {
    case 0: return 0;
    case 1: return UINT64_C(0xa00000221596882);
    case 2: return race->snapshot & ~UINT64_C(2);
    default: return race->snapshot ^ (UINT64_C(1) << 36);
    }
}
static void *replace_entry(void *opaque) {
    Race *race = opaque;
    for (unsigned i = 0; i < 8192; i++) {
        pthread_barrier_wait(&race->barrier);
        __atomic_store_n(race->entry, replacement_for(race, i), __ATOMIC_SEQ_CST);
        pthread_barrier_wait(&race->barrier);
    }
    return NULL;
}
static uint32_t offset21(int words) {
    return ((words & 0xffff) << 10) | ((words >> 16) & 31);
}
static void check_native(unsigned site, uint64_t snapshot, uint64_t initial,
                         bool expected)
{
    unsigned features, index = 2;
    __asm__("cpucfg %0,%1" : "=r"(features) : "r"(index));
    if (cpuinfo && !(features & (1u << 28))) return;
    uint32_t *native = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(native != MAP_FAILED);
    unsigned position[256], at = 5;
    for (unsigned i = 0; i < count; i++) {
        position[i] = at;
        at += code[i].op == LABEL ? 0 : code[i].op == RETURN ? 2 : 1;
    }
    assert(at * 4 < 4096);
    /* t2=argument address, t3=expected PTE, t7=zero RAM-base offset. */
    native[0] = 0x00150000 | 14 | (4 << 5);
    native[1] = 0x00150000 | 15 | (5 << 5);
    native[2] = 0x03800000 | 19;
    native[3] = 0x00150000 | 18 | (6 << 5);
    native[4] = 0x50000000 | offset21(position[starts[site]] - 4);
    for (unsigned i = 0; i < count; i++) {
        Insn x = code[i]; unsigned pos = position[i]; uint32_t word = 0;
        int delta;
        switch (x.op) {
        case LABEL: continue;
        case RETURN:
            native[pos] = 0x03800000 | 4 | (x.a << 10);
            native[pos + 1] = 0x4c000000 | (1 << 5);
            continue;
        case LISA_ANDI: word = 0x03400000 | x.a | x.b << 5 | x.c << 10; break;
        case LISA_ORI: word = 0x03800000 | x.a | x.b << 5 | x.c << 10; break;
        case LISA_ADD_D: word = 0x00108000 | x.a | x.b << 5 | x.c << 10; break;
        case LISA_SUB_D: word = 0x00118000 | x.a | x.b << 5 | x.c << 10; break;
        case LISA_MASKEQZ: word = 0x00130000 | x.a | x.b << 5 | x.c << 10; break;
        case LISA_AMCAS_DB_D: word = 0x385b8000 | x.a | x.b << 5 | x.c << 10; break;
        case LISA_LL_D: word = 0x22000000 | x.a | x.b << 5; break;
        case LISA_SC_D: word = 0x23000000 | x.a | x.b << 5; break;
        case LISA_DBAR: word = 0x38720000; break;
        case LISA_BEQZ: case LISA_BNEZ:
            delta = position[labels[x.b]] - pos;
            word = (x.op == LISA_BEQZ ? 0x40000000 : 0x44000000)
                   | x.a << 5 | offset21(delta);
            break;
        case LISA_BNE:
            delta = position[labels[x.c]] - pos;
            word = 0x5c000000 | x.b | x.a << 5 | (delta & 0xffff) << 10;
            break;
        case LISA_B:
            delta = position[labels[x.a]] - pos;
            word = 0x50000000 | ((delta & 0xffff) << 10)
                   | ((delta >> 16) & 0x3ff);
            break;
        default: assert(false);
        }
        native[pos] = word;
    }
    __builtin___clear_cache((char *)native, (char *)(native + at));
    uint64_t memory = initial;
    typedef unsigned (*Update)(uint64_t *, uint64_t, uint64_t);
    uint64_t flags = (parent_readonly ? 0 : 2) | (parent_missing ? 0 : PG_ACCESSED_MASK);
    assert(((Update)native)(&memory, snapshot, flags) == expected);
    assert(memory == (expected ? snapshot | PG_ACCESSED_MASK : initial));
    if (native_stress) {
        Race race = {.entry = &memory, .snapshot = snapshot};
        pthread_t writer;
        assert(!pthread_barrier_init(&race.barrier, NULL, 2));
        assert(!pthread_create(&writer, NULL, replace_entry, &race));
        for (unsigned i = 0; i < 8192; i++) {
            __atomic_store_n(&memory, snapshot, __ATOMIC_SEQ_CST);
            pthread_barrier_wait(&race.barrier);
            ((Update)native)(&memory, snapshot, flags);
            pthread_barrier_wait(&race.barrier);
            assert(__atomic_load_n(&memory, __ATOMIC_SEQ_CST) == replacement_for(&race, i));
        }
        assert(!pthread_join(writer, NULL));
        assert(!pthread_barrier_destroy(&race.barrier));
        printf("native concurrent replacement: 8192 iterations PASS\n");
    }
    munmap(native, 4096);
    checks++;
}
#endif

static void check_backend(unsigned backend)
{
    count = 0; next_label = 32;
    parent_readonly = parent_missing = false;
    cpuinfo = backend ? CPUINFO_LAMCAS : 0;
    printf("backend: %s\n", backend ? "AMCAS" : "LL/SC");
    HamtPtwAccessed accessed[4];
    IR2_OPND slow = la_new_label();
    for (unsigned i = 0; i < 4; i++) {
        starts[i] = count;
        gen_hamt_parent_accessed(&slow);
        la_out2i(LISA_ANDI, &la_t6, &la_t6, 2);
        gen_hamt_leaf_accessed(&accessed[i]);
        gen_hamt_leaf_write_permission();
        emit(RETURN, 1, 0, 0);
    }
    la_label(slow); emit(RETURN, 0, 0, 0);
    for (unsigned i = 0; i < 4; i++) {
        gen_hamt_accessed_update(&accessed[i], &slow);
    }
    for (unsigned i = 0; i < count; i++) {
        if (code[i].op == LABEL) labels[code[i].a] = i;
    }
    for (unsigned i = 0; i < 4; i++) {
        uint64_t old = UINT64_C(0x200004007) | PG_DIRTY_MASK;
        check(i, "already-accessed", old | PG_ACCESSED_MASK, 0, 0, true);
        check(i, "set-accessed", old, 0, 0, true);
        check(i, "already-accessed-unmapped", old | PG_ACCESSED_MASK,
              4, 0, true);
        check(i, "unmap-before-ll", old, 1, 0, false);
        check(i, "transition-before-sc", old, 2, UINT64_C(0xa00000221596882), false);
        check(i, "high-pfn-change", old, 1, old ^ (UINT64_C(1) << 36), false);
        check(i, "write-revoked", old, 2, old & ~UINT64_C(2), false);
        if (!cpuinfo) check(i, "lost-reservation", old, 3, 0, false);
#ifdef __loongarch__
        check_native(i, old, old, true);
        check_native(i, old | PG_ACCESSED_MASK, old | PG_ACCESSED_MASK, true);
        check_native(i, old, old ^ (UINT64_C(1) << 36), false);
#endif
    }
    check(0, "large-clean-slow-path", 0x200087, 0, 0, false);
    check(1, "large-dirty", 0x2000c7, 0, 0, true);
    check(2, "leaf-clean-slow-path", 0x4007, 0, 0, false);
    check(3, "accessed-clean-readonly", 0x4027, 0, 0, true);
    parent_readonly = true;
    check(0, "parent-revokes-write", 0x4067, 0, 0, true);
#ifdef __loongarch__
    check_native(0, 0x200087, 0x200087, false);
    check_native(1, 0x2000c7, 0x2000c7, true);
    check_native(2, 0x4007, 0x4007, false);
    check_native(3, 0x4027, 0x4027, true);
#endif
#ifdef __loongarch__
    parent_readonly = false;
    native_stress = true;
    check_native(0, 0x200004047, 0x200004047, true);
    native_stress = false;
#endif
    parent_missing = true;
    check(0, "missing-parent-accessed", 0x4067, 0, 0, false);
#ifdef __loongarch__
    check_native(0, 0x4067, 0x4067, false);
#endif
}

int main(void)
{
    for (unsigned backend = 0; backend < 2; backend++) {
        check_backend(backend);
    }
    printf("%u refill checks passed\n", checks);
}
'''
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)
    (path / 'test.c').write_text(program)
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11',
                    '-O2', '-pthread', str(path / 'test.c'), '-o', str(path / 'test')],
                   check=True)
    subprocess.run([str(path / 'test')], check=True)
