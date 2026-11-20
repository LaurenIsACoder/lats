#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise production TU exploration, rollback and spool decisions in C.

The decoder/code generator are replaced with deterministic TB fixtures. All
membership, retry, page validation and ownership decisions come from source.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
tu = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
save = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()
header = (root / "target/i386/latx/include/latx-tu.h").read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


def typedef(source, name):
    start = source.index("typedef ", source.index(name) - 15)
    return source[start:source.index("} " + name + ";", start) + len(name) + 3]


# A successful TU must reuse the mapping snapshot collected by its caller.
save_one_source = function(save, "static bool save_one_tu(")
cross_page_spool_source = function(
    save, "static AOTCrossPageTUResult aot_spool_cross_page_tu(")
single_page_save_source = function(save, "static bool save_tu_page(")
assert "aot_collect_tu_mappings(" not in save_one_source
assert cross_page_spool_source.count("aot_collect_tu_mappings(") == 1
assert single_page_save_source.count("aot_collect_tu_mappings(") == 1


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <glib.h>
typedef uint64_t target_ulong;
typedef uint64_t tb_page_addr_t;
typedef uint64_t ADDRX;
typedef int TU_TB_START_TYPE;
typedef struct CPUState { int unused; } CPUState;
#define TARGET_PAGE_SIZE 4096
#define TARGET_PAGE_MASK (~(uint64_t)(TARGET_PAGE_SIZE - 1))
#define AOT_TU_MAX_PAGES 8
static uint32_t option_tu_max_tb = 8;
static uint32_t option_aot_tu_max_tb = 8;
#define MAX_TB_IN_TU option_tu_max_tb
#define MAX_TB_IN_AOT_TU option_aot_tu_max_tb
#define IS_AOT_TB 1
#define CF_INVALID (1u << 31)
#define TU_TRANSLATE_OK 0
#define TU_TB_INDEX_NEXT 0
#define TU_TB_INDEX_TARGET 1
#define TU_TB_START_NORMAL 0
#define TU_TB_START_JMP 1
#define TU_TB_START_ENTRY 2
enum { IR1_TYPE_BRANCH, IR1_TYPE_JUMP, IR1_TYPE_CALLIN, IR1_TYPE_CALL,
       IR1_TYPE_NORMAL, IR1_TYPE_SYSCALL, IR1_TYPE_JUMPIN, IR1_TYPE_RET };
typedef struct TranslationBlock TranslationBlock;
struct separated_data {
    int last_ir1_type;
    uint64_t x86_next_pc, x86_target_pc;
    int tu_jmp[2];
    TranslationBlock *next_tb[2];
};
struct TranslationBlock {
    uint64_t pc, cs_base;
    uint64_t ram_page_addr[2];
    uintptr_t jmp_dest[2];
    uint32_t flags, cflags, bool_flags;
    struct { void *ptr; } tc;
    struct separated_data *s_data;
    int tu_jmp[2], id, icount;
};
static struct {
    TranslationBlock *tb_list[32];
    uint32_t tb_num, ir1_num_in_tu;
} tu_state, *tu_data = &tu_state;
static struct { void *code_gen_ptr, *data_gen_ptr; } tcg, *tcg_ctx = &tcg;
static unsigned char ram[64 * TARGET_PAGE_SIZE];
static FILE *out_tb_file;
static int option_split_tb = 1;
static int decode_fail = -1, invalid_ram = -1, add_second_page;
static int allocation_fail, write_fail, translate_fail;
static int allocated, freed, translated, resets, saved[32], rejects[4];
static uint64_t ghbr_pending, ghbr_emitted, ghbr_discarded, ghbr_rollbacks;
static struct { bool failed; } *aot_file_code_view;
static const uint8_t *aot_file_code_page(uint64_t pc)
{ (void)pc; abort(); }
#define MAX_IR1_IN_TU 8192
static int active_code;
static int option_aot_tu_pretranslate;
static unsigned int saved_groups[32];
static int group_count, change_mapping_after_explore;
static int graph_alloc_fail = -1, graph_alloc_count;
#define tb_page_addr0(tb) ((tb)->ram_page_addr[0])
#define tb_page_addr1(tb) ((tb)->ram_page_addr[1])
#define get_page(pc) ((pc) & TARGET_PAGE_MASK)
#define lsassert(x) assert(x)
#define qatomic_set(p, v) (*(p) = (v))
#define qatomic_read(p) (*(p))
static void *qemu_ram_addr_to_host(uint64_t ram_page_addr)
{ return ram_page_addr < sizeof(ram) ? ram + ram_page_addr : NULL; }
static bool aot_host_pc_in_main_ram(void *p)
{ return p >= (void *)ram && p < (void *)(ram + sizeof(ram)); }
static void *aot_ram_addr_to_host(uint64_t ram_page_addr)
{ return qemu_ram_addr_to_host(ram_page_addr); }
static bool aot_host_page_is_valid(void *p)
{ return aot_host_pc_in_main_ram(p); }
static uint64_t get_page_fingerprint(void *p) { return (uintptr_t)p; }
static bool inst_may_cross_page(uint64_t pc) { (void)pc; return false; }
static int get_max_insns(int cflags) { (void)cflags; return 128; }
static void clear_tu_rel_table(void) { active_code = 0; resets++; }
static uint64_t latxs_ghbr_stats_checkpoint(void) { return ghbr_pending; }
static void latxs_ghbr_stats_restore(uint64_t checkpoint)
{
    if (ghbr_pending > checkpoint) {
        ghbr_discarded += ghbr_pending - checkpoint;
        ghbr_rollbacks++;
    }
    ghbr_pending = checkpoint;
}
static void latxs_ghbr_stats_commit(void)
{
    ghbr_emitted += ghbr_pending;
    ghbr_pending = 0;
}
static void region_reset_search(uint32_t id) { (void)id; }
static void region_reset_tb(uint32_t id) { (void)id; }
static void *test_realloc_n(void *p, size_t n, size_t size)
{ return allocation_fail ? NULL : realloc(p, n * size); }
#define g_try_realloc_n test_realloc_n
static void *test_graph_alloc(size_t count, size_t size)
{
    if (graph_alloc_count++ == graph_alloc_fail) return NULL;
    return g_try_malloc_n(count, size);
}
#undef g_try_new
#define g_try_new(type, count) ((type *)test_graph_alloc(count, sizeof(type)))
static void tracked_free(void *p) { if (p) freed++; free(p); }
static TranslationBlock *aot_tb_create(CPUState *cpu, uint64_t pc,
        uintptr_t hva, TranslationBlock *orig, int max_insns, bool first)
{
    (void)cpu; (void)pc; (void)hva; (void)max_insns; (void)first;
    assert(!active_code);
    if (orig->id == decode_fail) return NULL;
    TranslationBlock *tb = malloc(sizeof(*tb) + sizeof(*tb->s_data));
    *tb = *orig;
    tb->s_data = (void *)(tb + 1);
    *tb->s_data = *orig->s_data;
    memset(tb->s_data->next_tb, 0, sizeof(tb->s_data->next_tb));
    tb->tc.ptr = NULL;
    if (add_second_page) {
        tb->ram_page_addr[1] = tb->ram_page_addr[0] + TARGET_PAGE_SIZE;
    }
    if (orig->id == invalid_ram) tb->ram_page_addr[0] = sizeof(ram);
    allocated++;
    return tb;
}
static void tu_push_back(TranslationBlock *tb)
{ if (tb) tu_data->tb_list[tu_data->tb_num++] = tb; }
static int tb_sort_cmp(const void *a, const void *b)
{
    const TranslationBlock *x = *(TranslationBlock * const *)a;
    const TranslationBlock *y = *(TranslationBlock * const *)b;
    return x->pc < y->pc ? -1 : x->pc > y->pc;
}
static void solve_tb_overlap(uint32_t n, TranslationBlock **t, int m)
{ (void)n; (void)t; (void)m; }
static void tu_optimize_ir1(CPUState *c, TranslationBlock **t, uint32_t n,
                          uint64_t base, uint32_t f, int cf)
{ (void)c; (void)t; (void)n; (void)base; (void)f; (void)cf; }
static int translate_tu(CPUState *cpu, uint32_t n, TranslationBlock **list)
{
    uint64_t checkpoint = latxs_ghbr_stats_checkpoint();

    (void)cpu;
    assert(!active_code && n && n <= MAX_TB_IN_TU);
    for (uint32_t i = 0; i < n; i++) {
        for (int k = 0; k < 2; k++) {
            TranslationBlock *next = list[i]->s_data->next_tb[k];
            bool found = !next;
            for (uint32_t j = 0; j < n; j++) found |= next == list[j];
            assert(found); /* no links into a discarded/saved TU */
        }
    }
    translated++;
    ghbr_pending += n;
    if (translate_fail) {
        latxs_ghbr_stats_restore(checkpoint);
        return -1;
    }
    if (change_mapping_after_explore) {
        for (uint32_t i = 0; i < n; i++) {
            if (list[i]->id == 1) {
                list[i]->ram_page_addr[0] = 2 * TARGET_PAGE_SIZE;
            }
        }
    }
    active_code = 1;
    return TU_TRANSLATE_OK;
}
static bool save_one_tu(TranslationBlock **list, int n,
                        const void *mappings,
                        uint32_t mapping_count, int *size, uint64_t *id)
{
    (void)mappings;
    assert(mapping_count > 0 && mapping_count <= AOT_TU_MAX_PAGES);
    assert(active_code);
    if (write_fail) return false;
    unsigned int group = 0;
    for (int i = 0; i < n; i++) {
        saved[list[i]->id]++;
        group |= 1u << list[i]->id;
    }
    saved_groups[group_count++] = group;
    *id = list[0]->id + 1;
    *size = sizeof(n);
    return fwrite(&n, sizeof(n), 1, out_tb_file) == 1;
}
'''

types = "\n".join(typedef(save, name) for name in (
    "AOTTUSavePage", "AOTTUMappingResult", "AOTTUSaveRef",
    "AOTTUSpoolRecord", "AOTCrossPageSpool"))
types += typedef(header, "AOTCrossPageTUResult")
types += typedef(tu, "AOTTUExplorePage")
types += typedef(tu, "AOTSpecKey")
types += typedef(tu, "AOTSpecState")
types += (
    "\ntypedef AOTCrossPageTUResult (*AOTCrossPageTUVisitor)(\n"
    "    TranslationBlock **, int, void *);\n"
)

bodies = "\n".join(function(tu, signature) for signature in (
    "static int get_tb_id(", "static bool aot_templates_share_tu(",
    "static bool aot_runtime_edge_supported(",
    "static int aot_find_runtime_target(",
    "static bool aot_tu_add_template_pages(",
    "static int get_aot_tb_id(", "static uintptr_t aot_template_hva("))
bodies += "\n".join(function(tu, signature) for signature in (
    "static guint aot_spec_key_hash(",
    "static gboolean aot_spec_key_equal(", "static void aot_spec_key_store(",
    "static bool aot_spec_key_claimed(", "static void aot_spec_commit(",
    "static void aot_spec_rollback(", "static bool aot_template_key_exists(",
    "static TranslationBlock *aot_speculative_successor("))
bodies += "\n#define free tracked_free\n"
bodies += function(tu, "static TranslationBlock *aot_tu_get_successor(")
bodies += "\n#undef free\n"
bodies += "\n".join(function(tu, signature) for signature in (
    "static TranslationBlock *get_ts_successor(",
    "static inline void get_ts_queue(",
    "static void ts_tb_explore(", "static int aot_full_pc_cmp("))
bodies += "\n#define free tracked_free\n"
bodies += function(tu, "static void release_cross_page_tu(")
bodies += "\n#undef free\n"
bodies += function(tu, "bool retranslate_by_tu_cross_page(")
bodies += "\n" + "\n".join(function(save, signature) for signature in (
    "static bool aot_cross_page_reserve(",
    "static AOTTUMappingResult aot_collect_tu_mappings(",
    "static void aot_cross_page_reset_translation(",
    "static AOTCrossPageTUResult aot_spool_cross_page_tu("))

main = r'''
static TranslationBlock templates[16], *ptrs[16];
static struct separated_data data[16];
static AOTCrossPageSpool spool;
static CPUState cpu;
static void init(int n, bool separate_pages)
{
    memset(&tu_state, 0, sizeof(tu_state));
    memset(templates, 0, sizeof(templates));
    memset(data, 0, sizeof(data));
    memset(saved, 0, sizeof(saved));
    memset(saved_groups, 0, sizeof(saved_groups));
    group_count = change_mapping_after_explore = 0;
    graph_alloc_fail = -1;
    graph_alloc_count = 0;
    option_tu_max_tb = 8;
    memset(rejects, 0, sizeof(rejects));
    memset(&spool, 0, sizeof(spool));
    spool.file = tmpfile(); assert(spool.file);
    decode_fail = invalid_ram = -1;
    add_second_page = allocation_fail = write_fail = translate_fail = 0;
    allocated = freed = translated = resets = active_code = 0;
    ghbr_pending = ghbr_emitted = ghbr_discarded = ghbr_rollbacks = 0;
    out_tb_file = NULL;
    for (size_t p = 0; p < sizeof(ram) / TARGET_PAGE_SIZE; p++) {
        memset(ram + p * TARGET_PAGE_SIZE, p, TARGET_PAGE_SIZE);
    }
    for (int i = 0; i < n; i++) {
        templates[i].id = i;
        templates[i].pc = separate_pages ? (2 * i + 1) * TARGET_PAGE_SIZE :
                                           TARGET_PAGE_SIZE + i * 16;
        templates[i].ram_page_addr[0] =
            separate_pages ? templates[i].pc : TARGET_PAGE_SIZE;
        templates[i].ram_page_addr[1] = -1;
        templates[i].s_data = &data[i];
        templates[i].bool_flags = 0x40;
        data[i].last_ir1_type = i + 1 < n ?
            (separate_pages ? IR1_TYPE_BRANCH : IR1_TYPE_JUMP) : IR1_TYPE_RET;
        if (i) {
            templates[i - 1].s_data->x86_target_pc = templates[i].pc;
            templates[i - 1].jmp_dest[1] = (uintptr_t)&templates[i];
        }
        ptrs[i] = &templates[i];
    }
}
static void finish(int n)
{
    assert(allocated == freed && !active_code && !tu_data->tb_num);
    assert(!ghbr_pending);
    assert(out_tb_file == NULL);
    for (int i = 0; i < n; i++) {
        assert(!templates[i].tc.ptr);
        assert(templates[i].bool_flags & 0x40);
    }
    fclose(spool.file); free(spool.records); free(spool.refs);
}
static void run(int n, bool ok)
{
    uintptr_t links[16][2];
    for (int i = 0; i < n; i++) {
        memcpy(links[i], templates[i].jmp_dest, sizeof(links[i]));
    }
    assert(retranslate_by_tu_cross_page(&cpu, ptrs, n,
                                       aot_spool_cross_page_tu, &spool) == ok);
    for (int i = 0; i < n; i++) {
        assert(!memcmp(links[i], templates[i].jmp_dest, sizeof(links[i])));
    }
}
int main(void)
{
    /* Search the whole equal-key range, including both ends of the array. */
    init(8, true);
    for (int i = 0; i < 8; i++) {
        templates[i].pc = TARGET_PAGE_SIZE;
    }
    for (int source = 0; source < 8; source += 7) {
        data[source].last_ir1_type = IR1_TYPE_BRANCH;
        templates[source].s_data->x86_target_pc = TARGET_PAGE_SIZE;
        for (int target = 0; target < 8; target++) {
            templates[source].jmp_dest[1] = (uintptr_t)&templates[target];
            assert(aot_find_runtime_target(ptrs, 8, source, 1,
                                           TARGET_PAGE_SIZE) == target);
        }
        /* An uncollected address must not be dereferenced or substituted. */
        templates[source].jmp_dest[1] = 2;
        assert(aot_find_runtime_target(ptrs, 8, source, 1,
                                       TARGET_PAGE_SIZE) == -1);
    }
    finish(8);

    /* Reused CR3 and equal PCs: the runtime edge selects C, not B. */
    init(3, true);
    templates[2].pc = templates[1].pc;
    data[1].last_ir1_type = IR1_TYPE_RET;
    templates[0].jmp_dest[1] = (uintptr_t)&templates[2];
    run(3, true);
    assert(group_count == 2 && saved_groups[0] == 0x5);
    assert(saved_groups[1] == 0x2);
    for (int i = 0; i < 3; i++) assert(saved[i] == 1);
    finish(3);

    /* Missing, invalidating, or uncollected edges never fall back to PC. */
    for (int missing = 0; missing < 4; missing++) {
        init(2, true);
        uintptr_t links[] = { 0, 1, (uintptr_t)&templates[1] | 1,
                              (uintptr_t)&templates[15] };
        templates[0].jmp_dest[1] = links[missing];
        run(2, true);
        assert(translated == 2 && saved_groups[0] == 1);
        assert(saved_groups[1] == 2);
        finish(2);
    }

    /* A failed linked target must not be replaced by an equal-PC candidate. */
    init(3, true);
    templates[2].pc = templates[1].pc;
    data[1].last_ir1_type = IR1_TYPE_RET;
    decode_fail = 1;
    run(3, true);
    assert(group_count == 2 && saved_groups[0] == 1);
    assert(saved_groups[1] == 4 && !saved[1]);
    finish(3);

    /* Read links on demand, not from an eager snapshot. */
    init(2, true);
    assert(aot_find_runtime_target(ptrs, 2, 0, 1,
                                   templates[0].s_data->x86_target_pc) == 1);
    templates[0].jmp_dest[1] = 0;
    assert(aot_find_runtime_target(ptrs, 2, 0, 1,
                                   templates[0].s_data->x86_target_pc) == -1);
    int source_ids[9] = { 0 };
    ts_tb_explore(&cpu, ptrs, 2, 0, 0, 0, true, MAX_TB_IN_TU, source_ids,
                  NULL);
    assert(tu_data->tb_num == 1);
    release_cross_page_tu(ptrs, 2, false);
    finish(2);

    /* Bad source/target identity is rejected even with a raw pointer link. */
    for (int bad = 0; bad < 6; bad++) {
        init(2, true);
        if (bad == 0) templates[0].cflags |= CF_INVALID;
        if (bad == 1) templates[1].cflags |= CF_INVALID;
        if (bad == 2) templates[1].pc++;
        if (bad == 3) templates[1].flags++;
        if (bad == 4) templates[1].cs_base++;
        if (bad == 5) templates[1].cflags++;
        assert(aot_find_runtime_target(ptrs, 2, 0, 1,
                                       templates[0].s_data->x86_target_pc) ==
           -1);
        finish(2);
    }

    /* Raw JIT templates do not own decoded TU branch metadata. */
    init(2, true);
    target_ulong decoded_pc = templates[1].pc;
    data[0].x86_next_pc = data[0].x86_target_pc = UINT64_MAX;
    assert(aot_find_runtime_target(ptrs, 2, 0, 1, decoded_pc) == 1);
    templates[0].s_data = NULL;
    assert(aot_find_runtime_target(ptrs, 2, 0, 1, decoded_pc) == 1);
    assert(aot_find_runtime_target(ptrs, 2, 0, 1, decoded_pc + 1) == -1);
    assert(aot_find_runtime_target(ptrs, 2, -1, 1, decoded_pc) == -1);
    assert(aot_find_runtime_target(ptrs, 2, 2, 1, decoded_pc) == -1);
    assert(aot_find_runtime_target(ptrs, 2, 0, -1, decoded_pc) == -1);
    assert(aot_find_runtime_target(ptrs, 2, 0, 2, decoded_pc) == -1);
    finish(2);

    /* Direct and sequential runtime edges may introduce a new page. */
    int exit_types[] = { IR1_TYPE_JUMP, IR1_TYPE_CALLIN, IR1_TYPE_CALL,
                         IR1_TYPE_NORMAL, IR1_TYPE_SYSCALL };
    for (size_t i = 0; i < sizeof(exit_types) / sizeof(exit_types[0]); i++) {
        init(2, true);
        data[0].last_ir1_type = exit_types[i];
        templates[0].s_data->x86_next_pc = templates[1].pc;
        if (exit_types[i] != IR1_TYPE_JUMP) {
            templates[0].jmp_dest[0] = (uintptr_t)&templates[1];
        }
        run(2, true);
        assert(translated == 1 && group_count == 1);
        assert(saved_groups[0] == 3);
        finish(2);
    }

    /* A JMP may return to a page already introduced by the TU. */
    init(3, true);
    templates[2].pc = templates[0].pc + 16;
    templates[2].ram_page_addr[0] = templates[0].ram_page_addr[0];
    data[1].last_ir1_type = IR1_TYPE_JUMP;
    templates[1].s_data->x86_target_pc = templates[2].pc;
    run(3, true);
    assert(translated == 1 && saved_groups[0] == 7);
    finish(3);

    /* A valid TU is not shrunk or translated again. */
    init(8, true); run(8, true);
    assert(translated == 1 && resets == 1 && spool.record_count == 1);
    assert(ghbr_emitted == 8 && !ghbr_discarded && !ghbr_rollbacks);
    for (int i = 0; i < 8; i++) assert(saved[i] == 1);
    finish(8);

    /* A can reach C directly: reject B, but retain A+C+D as one TU. */
    init(4, false);
    data[0].last_ir1_type = IR1_TYPE_BRANCH;
    templates[0].s_data->x86_next_pc = templates[1].pc;
    templates[0].s_data->x86_target_pc = templates[2].pc;
    templates[1].ram_page_addr[0] = 2 * TARGET_PAGE_SIZE;
    run(4, true);
    assert(translated == 2 && group_count == 2);
    assert(saved_groups[0] == 0xd && saved_groups[1] == 0x2);
    assert(!spool.mapping_rejects[AOT_TU_MAPPING_CONFLICT]);
    for (int i = 0; i < 4; i++) assert(saved[i] == 1);
    finish(4);

    /* The second branch arm must see the first arm's newly added page. */
    init(3, true);
    templates[2].pc = templates[1].pc + 16;
    data[0].last_ir1_type = IR1_TYPE_BRANCH;
    data[1].last_ir1_type = IR1_TYPE_RET;
    templates[0].s_data->x86_next_pc = templates[1].pc;
    templates[0].s_data->x86_target_pc = templates[2].pc;
    templates[0].jmp_dest[0] = (uintptr_t)&templates[1];
    templates[0].jmp_dest[1] = (uintptr_t)&templates[2];
    run(3, true);
    assert(translated == 2 && saved_groups[0] == 0x3);
    assert(saved_groups[1] == 0x4);
    assert(!spool.mapping_rejects[AOT_TU_MAPPING_CONFLICT]);
    finish(3);

    /* Equal lookup keys: skip a conflicting candidate and use the next. */
    init(3, false);
    templates[2].pc = templates[1].pc;
    templates[1].ram_page_addr[0] = 2 * TARGET_PAGE_SIZE;
    data[1].last_ir1_type = IR1_TYPE_RET;
    run(3, true);
    assert(translated == 2 && saved_groups[0] == 0x5);
    assert(saved_groups[1] == 0x2);
    for (int i = 0; i < 3; i++) assert(saved[i] == 1);
    finish(3);

    /* A failed decode must not hide another candidate at the same PC. */
    init(3, false);
    templates[2].pc = templates[1].pc;
    data[1].last_ir1_type = IR1_TYPE_RET;
    decode_fail = 1;
    run(3, true);
    assert(translated == 1 && saved_groups[0] == 0x5 && !saved[1]);
    finish(3);

    /* Rejecting the second page must not commit the first missing page. */
    init(2, true);
    AOTTUExplorePage pages[AOT_TU_MAX_PAGES];
    uint32_t page_count = 0;
    templates[0].pc = 4 * TARGET_PAGE_SIZE;
    assert(aot_tu_add_template_pages(&templates[0], pages, &page_count));
    templates[1].pc = 3 * TARGET_PAGE_SIZE;
    templates[1].ram_page_addr[1] = 2 * TARGET_PAGE_SIZE;
    assert(!aot_tu_add_template_pages(&templates[1], pages, &page_count));
    assert(page_count == 1 && pages[0].guest_page_addr == templates[0].pc);
    templates[0].pc = TARGET_PAGE_SIZE;
    page_count = 0;
    assert(aot_tu_add_template_pages(&templates[0], pages, &page_count));
    templates[1].ram_page_addr[1] = (uint64_t)-1;
    templates[1].pc = templates[0].pc + 16;
    templates[1].ram_page_addr[0] = templates[0].ram_page_addr[0];
    decode_fail = 1;
    assert(!aot_tu_get_successor(ptrs, 2, 0, &cpu, templates[1].pc,
            0, 0, 128, TU_TB_START_NORMAL, &templates[0], 8,
            pages, &page_count, &templates[0], 0, TU_TB_INDEX_TARGET, NULL,
            NULL));
    assert(page_count == 1 && !(templates[1].bool_flags & IS_AOT_TB));
    finish(2);

    /* A previously saved candidate must not hide an unused equal-key TB. */
    init(3, false);
    templates[2].pc = templates[1].pc;
    templates[1].bool_flags |= IS_AOT_TB;
    page_count = 0;
    assert(aot_tu_add_template_pages(&templates[0], pages, &page_count));
    assert(get_aot_tb_id(ptrs, 0, 2, templates[1].pc, 0, 0,
                        &templates[0], pages, page_count) == 2);
    finish(3);

    /* Single-page AOT keeps its existing branch and page-boundary policy. */
    init(3, false);
    data[0].last_ir1_type = IR1_TYPE_BRANCH;
    templates[0].s_data->x86_next_pc = templates[1].pc;
    templates[0].s_data->x86_target_pc = templates[2].pc;
    ts_tb_explore(&cpu, ptrs, 3, 0, TARGET_PAGE_SIZE,
                  (uintptr_t)(ram + TARGET_PAGE_SIZE), false, MAX_TB_IN_TU,
                  NULL, NULL);
    assert(tu_data->tb_num == 3);
    assert(tu_data->tb_list[0]->s_data->next_tb[0]->id == 1);
    assert(tu_data->tb_list[0]->s_data->next_tb[1]->id == 2);
    release_cross_page_tu(ptrs, 3, false);
    finish(3);
    init(2, true);
    ts_tb_explore(&cpu, ptrs, 2, 0, TARGET_PAGE_SIZE,
                  (uintptr_t)(ram + TARGET_PAGE_SIZE), false, MAX_TB_IN_TU,
                  NULL, NULL);
    assert(tu_data->tb_num == 1);
    release_cross_page_tu(ptrs, 2, false);
    finish(2);

    /* A decoded rejection also leaves the set and ownership unchanged. */
    init(2, true);
    templates[1].pc = templates[0].pc + 16;
    templates[1].ram_page_addr[0] = templates[0].ram_page_addr[0];
    page_count = 0;
    assert(aot_tu_add_template_pages(&templates[0], pages, &page_count));
    invalid_ram = 1;
    assert(!aot_tu_get_successor(ptrs, 2, 0, &cpu, templates[1].pc,
            0, 0, 128, TU_TB_START_NORMAL, &templates[0], 8,
            pages, &page_count, &templates[0], 0, TU_TB_INDEX_TARGET, NULL,
            NULL));
    assert(page_count == 1 && !(templates[1].bool_flags & IS_AOT_TB));
    finish(2);

    /* Final validation still catches changes after exploration and retries. */
    init(2, false);
    change_mapping_after_explore = 1;
    run(2, true);
    assert(spool.mapping_rejects[AOT_TU_MAPPING_CONFLICT] == 1);
    assert(translated == 3 && saved[0] == 1 && saved[1] == 1);
    assert(ghbr_emitted == 2 && ghbr_discarded == 2 && ghbr_rollbacks == 1);
    finish(2);

    /* Actual decoded TBs touch pages absent from the original templates. */
    init(8, true); add_second_page = 1; run(8, true);
    assert(!spool.mapping_rejects[AOT_TU_MAPPING_PAGE_LIMIT]);
    assert(spool.record_count == 2 && translated == 2);
    for (int i = 0; i < 8; i++) assert(saved[i] == 1);
    finish(8);

    /* Identical physical aliases share one canonical virtual-page record. */
    init(8, false);
    memcpy(ram + 2 * TARGET_PAGE_SIZE, ram + TARGET_PAGE_SIZE,
           TARGET_PAGE_SIZE);
    for (int i = 0; i < 8; i++) {
        templates[i].ram_page_addr[0] += (i % 2) * TARGET_PAGE_SIZE;
    }
    run(8, true);
    assert(translated == 1 && spool.record_count == 1 && spool.ref_count == 1);
    assert(spool.refs[0].ram_page_addr == TARGET_PAGE_SIZE);
    assert(!spool.mapping_rejects[AOT_TU_MAPPING_CONFLICT]);
    for (int i = 0; i < 8; i++) {
        assert(saved[i] == 1);
        assert(templates[i].ram_page_addr[0] == (1 + i % 2) * TARGET_PAGE_SIZE);
    }
    finish(8);

    /* A difference outside the TB bytes still rejects the page alias. */
    init(2, false);
    memcpy(ram + 2 * TARGET_PAGE_SIZE, ram + TARGET_PAGE_SIZE,
           TARGET_PAGE_SIZE);
    ram[3 * TARGET_PAGE_SIZE - 1] ^= 1;
    templates[1].ram_page_addr[0] += TARGET_PAGE_SIZE;
    run(2, true);
    assert(!spool.mapping_rejects[AOT_TU_MAPPING_CONFLICT]);
    assert(spool.record_count == 2 && translated == 2);
    assert(saved[0] == 1 && saved[1] == 1);
    finish(2);

    /* Invalid RAM on an alias must not be dereferenced by memcmp. */
    init(2, false); invalid_ram = 1; run(2, true);
    assert(spool.mapping_rejects[AOT_TU_MAPPING_INVALID_RAM]);
    assert(saved[0] == 1 && saved[1] == 0);
    finish(2);

    /* A chain of incompatible pages stops at each conflicting successor. */
    init(8, false);
    for (int i = 0; i < 8; i++) {
        templates[i].ram_page_addr[0] += (i % 2) * TARGET_PAGE_SIZE;
    }
    run(8, true);
    assert(!spool.mapping_rejects[AOT_TU_MAPPING_CONFLICT]);
    assert(translated == 8 && spool.record_count == 8);
    for (int i = 0; i < 8; i++) assert(saved[i] == 1);
    finish(8);

    /* An invalid member must not discard its healthy neighbours. */
    init(8, true); invalid_ram = 3; run(8, true);
    assert(spool.mapping_rejects[AOT_TU_MAPPING_INVALID_RAM]);
    for (int i = 0; i < 8; i++) assert(saved[i] == (i != 3));
    assert(translated <= 8 * 4);
    finish(8);

    /* Check both arms at the TB budget boundary (no max_tb + 1). */
    init(8, false);
    for (int i = 0; i < 6; i++) {
        data[i].last_ir1_type = IR1_TYPE_BRANCH;
        templates[i].s_data->x86_next_pc = templates[i + 1].pc;
        templates[i].s_data->x86_target_pc = templates[i + 2].pc;
        templates[i].ram_page_addr[0] += (i % 2) * TARGET_PAGE_SIZE;
    }
    run(8, true);
    for (int i = 0; i < 8; i++) assert(saved[i] == 1);
    finish(8);

    /* Equal lookup keys with different CS bases remain separate. */
    init(3, false);
    templates[2].pc = templates[0].pc;
    templates[2].cs_base = 1;
    data[1].last_ir1_type = IR1_TYPE_RET;
    templates[1].ram_page_addr[0] += TARGET_PAGE_SIZE;
    run(3, true);
    for (int i = 0; i < 3; i++) assert(saved[i] == 1);
    finish(3);

    init(1, true); invalid_ram = 0; run(1, true);
    assert(!saved[0] && translated == 1);
    finish(1);
    init(1, true); decode_fail = 0; run(1, true);
    assert(!translated && !saved[0]);
    finish(1);
    init(8, true); decode_fail = 3; run(8, true);
    for (int i = 0; i < 8; i++) assert(saved[i] == (i != 3));
    finish(8);

    /* Resource failures are fatal, not another geometric retry. */
    for (int allocation = 0; allocation < 1; allocation++) {
        init(8, true);
        graph_alloc_fail = allocation;
        run(8, false);
        assert(!translated && !spool.record_count);
        finish(8);
    }
    init(8, true); allocation_fail = 1; run(8, false);
    assert(translated == 1 && resets == 1 && !spool.record_count);
    assert(!ghbr_emitted && ghbr_discarded == 8 && ghbr_rollbacks == 1);
    finish(8);
    init(8, true); write_fail = 1; run(8, false);
    assert(translated == 1 && resets == 1 && !spool.record_count);
    assert(!ghbr_emitted && ghbr_discarded == 8 && ghbr_rollbacks == 1);
    finish(8);
    init(8, true); translate_fail = 1; run(8, false);
    assert(translated == 1 && !resets);
    assert(!ghbr_emitted && ghbr_discarded == 8 && ghbr_rollbacks == 1);
    finish(8);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="aot-tu-shrink-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(prefix + types + bodies + main)
    try:
        glib_flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    except (OSError, subprocess.CalledProcessError):
        print("SKIP: GLib development files and pkg-config are required")
        raise SystemExit(77)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-Wno-sign-compare",
                    "-Wno-unused-parameter"] +
                   shlex.split(os.environ.get("CFLAGS", "")) + [str(cfile),
                    "-o", str(binary)] + glib_flags, check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=30)
print("PASS: TU shrink retries preserve coverage, ownership and fatal errors")
