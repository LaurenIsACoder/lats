#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run the production TU publication routines on synthetic host objects.

The fixture supplies hash, physical-page and host-PC registries.
Usage: python3 tests/latx/test-tu-publication.py [source-root]
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
maintenance = (root / "accel/tcg/tb-maint.c").read_text()
tu = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
tb_header = (root / "include/exec/translation-block.h").read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CONFIG_LATX_TU 1
#define CONFIG_LATX_PRINT 1
#define CF_INVALID 1u
#define CF_NO_ACTIVE 2u
#define CF_PCREL 4u
#define IS_TU_TB 1u
#define HF_CPL_MASK 3u
#define MF_MTCC_ID_SHIFT 0
#define MF_MTCC_ID_MASK 1u
#define qatomic_read(p) (*(p))
#define qatomic_set(p, value) (*(p) = (value))
#define unlikely(a) (a)
#define tcg_debug_assert assert
#define g_assert assert

typedef int64_t tb_page_addr_t;
typedef struct TranslationBlock TranslationBlock;
typedef struct {
    unsigned cpu_index;
    void *env_ptr;
} CPUState;

enum { TU_TB_MODE_BROKEN = 1, BAD_TB, TU_TB_MODE_PRIVATE };
struct separated_data {
    TranslationBlock *next_tb[2];
    int tu_tb_mode;
    bool is_invalidate;
};
struct TranslationBlock {
    uint64_t pc, cs_base;
    tb_page_addr_t page_addr[2];
    unsigned flags, cflags, bool_flags, misc_flags;
    struct separated_data *s_data;
    bool host_record, page_record, shadow_record, ras_link;
    unsigned jump_unlinks, entry_poisoned;
    int jmp_lock;
};

struct qht {
    TranslationBlock *entries[8];
};
static struct {
    struct qht htable[2];
    unsigned tb_phys_invalidate_count;
} tb_ctx;
static CPUState cpu;
static CPUState *current_cpu = &cpu;
static unsigned mtcc;
static int tb_trans_cnt[8];

static unsigned tb_cflags(const TranslationBlock *tb) { return tb->cflags; }
static bool tb_is_tu(const TranslationBlock *tb)
{
    return tb->bool_flags & IS_TU_TB;
}
static bool tcg_tb_has_private_s_data(const TranslationBlock *tb)
{
    return tb->s_data != NULL;
}

static tb_page_addr_t tb_page_addr0(const TranslationBlock *tb)
{
    return tb->page_addr[0];
}

static tb_page_addr_t tb_page_addr1(const TranslationBlock *tb)
{
    return tb->page_addr[1];
}

static uint32_t tb_hash_func(tb_page_addr_t p, uint64_t pc,
                            unsigned flags, uint64_t cs_base, unsigned cflags)
{
    return p ^ pc ^ flags ^ cs_base ^ cflags;
}

static unsigned get_mask_from_miscflags(const TranslationBlock *tb,
                                        unsigned mask, unsigned shift)
{
    return (tb->misc_flags & mask) >> shift;
}

static bool qemu_tcg_mtcc_enabled(void) { return mtcc != 0; }
static unsigned curr_mtcc(void *env) { (void)env; return mtcc; }
static void assert_memory_lock(void) {}
static void qemu_thread_jit_write(void) {}
static void qemu_thread_jit_execute(void) {}

static void tb_record(TranslationBlock *tb)
{
    assert(!tb->page_record);
    tb->page_record = true;
}

static void tb_remove(TranslationBlock *tb)
{
    assert(tb->page_record);
    tb->page_record = false;
}

static void tcg_tb_insert(TranslationBlock *tb)
{
    assert(!tb->host_record);
    tb->host_record = true;
}

static void tcg_tb_remove(TranslationBlock *tb)
{
    assert(tb->host_record);
    tb->host_record = false;
}
'''

fixture += function(maintenance, "static bool tb_cmp(")
fixture += function(tb_header, "static inline bool tb_is_lookup_private(")
fixture += r'''
static TranslationBlock **prepared;
static size_t prepared_count;

static bool qht_insert(struct qht *table, void *ptr, uint32_t hash,
                       void **existing)
{
    TranslationBlock *tb = ptr;
    (void)hash;

    for (size_t i = 0; i < prepared_count; i++) {
        TranslationBlock *member = prepared[i];

        assert(member->host_record);
        assert(member->s_data->next_tb[0] == (i ? prepared[i - 1] : NULL));
        assert(member->s_data->next_tb[1] ==
               (i + 1 < prepared_count ? prepared[i + 1] : NULL));
    }
    assert(tb->page_record);
    for (size_t i = 0; i < ARRAY_SIZE(table->entries); i++) {
        if (table->entries[i] && tb_cmp(table->entries[i], tb)) {
            *existing = table->entries[i];
            return false;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(table->entries); i++) {
        if (!table->entries[i]) {
            table->entries[i] = tb;
            *existing = NULL;
            return true;
        }
    }
    abort();
}

static bool qht_remove(struct qht *table, const void *tb, uint32_t hash)
{
    (void)hash;
    for (size_t i = 0; i < ARRAY_SIZE(table->entries); i++) {
        if (table->entries[i] == tb) {
            table->entries[i] = NULL;
            return true;
        }
    }
    return false;
}

static void flaged_shadow_htable_insert(TranslationBlock *tb)
{
    assert(tb->page_record);
    tb->shadow_record = true;
}

static void flaged_shadow_htable_remove(TranslationBlock *tb)
{
    tb->shadow_record = false;
}

static void qemu_spin_lock(int *lock)
{
    assert(!*lock);
    *lock = 1;
}

static void qemu_spin_unlock(int *lock)
{
    assert(*lock);
    *lock = 0;
}

static void hamt_poison_tb_entry(TranslationBlock *tb)
{
    assert(tb->jmp_lock);
    tb->entry_poisoned++;
}

static void latxs_ras_unlink_target_locked(TranslationBlock *tb)
{
    assert(tb->jmp_lock && (tb->cflags & CF_INVALID));
}

static void tb_jmp_cache_inval_tb(TranslationBlock *tb)
{
    assert(tb->cflags & CF_INVALID);
}

static void latxs_ras_unlink(TranslationBlock *tb)
{
    tb->ras_link = false;
}

static void tb_remove_from_jmp_list(TranslationBlock *tb, unsigned edge)
{
    assert(edge < 2 && (tb->cflags & CF_INVALID));
    tb->jump_unlinks++;
}

static void tb_jmp_unlink(TranslationBlock *tb)
{
    assert(tb->jump_unlinks == 2);
}
'''
fixture += function(maintenance, "TranslationBlock *tb_link_page(")
fixture += function(maintenance, "static void do_tb_phys_invalidate(")
fixture += function(maintenance, "static void tb_phys_invalidate__locked(")
fixture += function(tu, "static TranslationBlock *register_tu(")

fixture += r'''
static TranslationBlock block(uint64_t pc, struct separated_data *data)
{
    return (TranslationBlock) {
        .pc = pc,
        .page_addr = { 0x2000 + pc, -1 },
        .flags = HF_CPL_MASK,
        .misc_flags = mtcc,
        .bool_flags = data ? IS_TU_TB : 0,
        .s_data = data,
        .ras_link = data != NULL,
    };
}

static TranslationBlock *lookup(const TranslationBlock *key)
{
    struct qht *table = &tb_ctx.htable[mtcc];

    for (size_t i = 0; i < ARRAY_SIZE(table->entries); i++) {
        if (table->entries[i] && tb_cmp(table->entries[i], key)) {
            return table->entries[i];
        }
    }
    return NULL;
}

static TranslationBlock *publish(TranslationBlock **list, size_t count,
                                  size_t entry)
{
    prepared = list;
    prepared_count = count;
    TranslationBlock *result = register_tu(&cpu, list[entry], count, list);
    prepared_count = 0;
    return result;
}

static void test_singleton_collision(void)
{
    struct separated_data data = { 0 };
    TranslationBlock canonical = block(0x100, NULL);
    TranslationBlock ordinary = block(0x100, NULL);
    TranslationBlock singleton = block(0x100, &data);
    TranslationBlock *list[] = { &singleton };

    mtcc = 0;
    assert(tb_link_page(&canonical, false) == &canonical);
    assert(tb_link_page(&ordinary, false) == &canonical);
    assert(!ordinary.page_record && !ordinary.shadow_record);
    assert(publish(list, ARRAY_SIZE(list), 0) == &canonical);
    assert(!tb_is_lookup_private(&singleton));
    assert(!singleton.host_record && !singleton.page_record);
    assert(!singleton.shadow_record && !singleton.ras_link);
    assert(lookup(&singleton) == &canonical);
    assert(canonical.page_record && canonical.shadow_record);
    tb_phys_invalidate__locked(&canonical);
    assert(lookup(&singleton) == NULL);
}

static void test_multi_member(bool collide, bool entry_collision)
{
    struct separated_data data[3] = { 0 };
    mtcc = 1;
    TranslationBlock canonical = block(0x200, NULL);
    TranslationBlock first = block(0x100, &data[0]);
    TranslationBlock middle = block(0x200, &data[1]);
    TranslationBlock last = block(0x300, &data[2]);
    TranslationBlock *list[] = { &first, &middle, &last };
    size_t entry_index = entry_collision ? 1 : 0;
    TranslationBlock *entry = list[entry_index];
    unsigned before = tb_ctx.tb_phys_invalidate_count;

    if (collide) {
        assert(tb_link_page(&canonical, false) == &canonical);
    }
    assert(publish(list, ARRAY_SIZE(list), entry_index) ==
           (entry_collision ? &canonical : entry));
    assert(lookup(&first) == &first && lookup(&last) == &last);
    assert(lookup(&middle) == (collide ? &canonical : &middle));
    assert(tb_is_lookup_private(&middle) == collide);
    assert(middle.host_record && middle.page_record && middle.ras_link);
    assert(middle.shadow_record == !collide);
    assert(first.shadow_record && last.shadow_record);

    tb_phys_invalidate__locked(&middle);
    assert(tb_ctx.tb_phys_invalidate_count == before + ARRAY_SIZE(list));
    for (size_t i = 0; i < ARRAY_SIZE(list); i++) {
        TranslationBlock *tb = list[i];
        assert(tb->cflags & CF_INVALID);
        assert(tb->s_data->is_invalidate && tb->host_record);
        assert(!tb->page_record && !tb->shadow_record && !tb->ras_link);
        assert(tb->entry_poisoned == 1 && tb->jump_unlinks == 2);
    }
    assert(!lookup(&first) && !lookup(&last));
    if (collide) {
        tb_phys_invalidate__locked(&middle);
        assert(tb_ctx.tb_phys_invalidate_count == before + ARRAY_SIZE(list));
        assert(middle.entry_poisoned == 1 && middle.jump_unlinks == 2);
        assert(!(canonical.cflags & CF_INVALID));
        assert(canonical.page_record && canonical.shadow_record);
        assert(lookup(&canonical) == &canonical);
        tb_phys_invalidate__locked(&canonical);
    }
}

int main(void)
{
    test_singleton_collision();
    test_multi_member(false, false);
    test_multi_member(true, false);
    test_multi_member(true, true);
    puts("PASS: TU publication order, canonical entries and private member retirement");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="latx-tu-publication-") as temp:
    source = Path(temp) / "test.c"
    executable = Path(temp) / "test"
    source.write_text(fixture)
    compiler = shlex.split(os.environ.get("CC", "cc"))
    subprocess.run(
        compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(executable)],
        check=True, timeout=60,
    )
    subprocess.run([str(executable)], check=True, timeout=15)
