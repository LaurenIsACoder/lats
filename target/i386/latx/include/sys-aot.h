/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SYS_AOT_H
#define SYS_AOT_H

#include "qemu/osdep.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "latx-options.h"

#include "target/i386/latx/include/latx-config.h"
#include "target/i386/latx/include/translate.h"
#include "exec/exec-all.h"
#include "tcg/tcg.h"

/*
 * Informational AOT messages (progress, statistics, merges, save
 * hand-offs) are verbose-only. Warnings and errors keep using
 * fprintf(stderr, ...) directly so they stay visible with verbose=off.
 */
#define aot_verbose_log(...)                 \
    do {                                     \
        if (option_verbose) {                \
            fprintf(stderr, __VA_ARGS__);    \
        }                                    \
    } while (0)

#ifdef CONFIG_LATX_DEBUG_AOT
#define DEBUG_AOT 1
#else
#define DEBUG_AOT 0
#endif

#include "sys-aot-format.h"

/*
 * Public AOT declarations are grouped by module owner. Keep helper-only
 * interfaces in module-local source files or narrower headers.
 */

/* Cross-module runtime state. */
extern int in_aot_translate;

static inline bool aot_guest_addr_fits_s32(uint64_t guest_addr)
{
    return guest_addr <= UINT64_C(0x7fffffff) ||
           guest_addr >= UINT64_C(0xffffffff80000000);
}

/* Debug counters shared by load, save and validation paths. */
#if DEBUG_AOT
extern uint64_t cpu_loop_exec_tb_time;
extern uint64_t loop_time;
extern uint64_t exec_time;
extern uint64_t exec_count;
extern uint64_t out_tb_file_tb_count;
extern uint64_t tb_hash_lookup_count;
extern uint64_t tb_hash_hit_count;
extern uint64_t cmp_count;
extern uint64_t cmp_tb_count;
extern uint64_t load_aot_time_counter;
extern uint64_t find_time;
extern uint64_t recover_time;
extern uint64_t one_ins_tb;

static inline long long get_tsc(void)
{
    long long r;
#ifdef __loongarch__
    asm volatile(
    "rdtime.d %0, $r0 \n\t"
    :"=r"(r)
    :
    :"memory"
    );
#else
    /* On x86, use rdtsc */
    unsigned int lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    r = ((long long)hi << 32) | lo;
#endif
    return r;
}

#endif

/* Runtime lifecycle, save triggers and load entry points. */
void mk_aot_dir(void);
void save_aot_tb(CPUArchState *env, TranslationBlock *tb, int search_size, tb_page_addr_t phys_pc);
uint64_t find_page(ADDR hva);
int recover_tb_head(TranslationBlock *curr_tb, aot_block *aot_tb);
TranslationBlock *load_aot(CPUState *cpu, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags, ENV *lsenv);
void check_aot_tb(TranslationBlock *curr_tb, TranslationBlock *tmp_tb);
bool check_ir2(TranslationBlock *curr_tb, aot_block *file_tb);
void aot_init(void);
void aot_save_on_tb_flush(int mtcc_id);
void aot_save_on_shutdown(void);

/* Performance instrumentation. */
void aot_perf_init_from_env(void);
void aot_perf_emit_run_context(int aot_enabled, int aot_load_enabled,
        int aot_save_enabled);
void aot_perf_emit_summary(void);
uint64_t aot_perf_now_ns(void);
bool aot_perf_enabled(void);
void aot_perf_record_load_file(uint32_t aot_mode, const char *file_name,
        uint64_t file_size, uint32_t page_count, uint32_t tb_count,
        bool integrity_ok, uint64_t mmap_ns);
void aot_perf_record_build_index(uint32_t aot_mode, uint32_t page_count,
        uint32_t hash_size, uint64_t bucket_collision_count,
        uint32_t linear_probe_max, uint64_t build_ns);
void aot_perf_record_lookup(bool hit, uint32_t probe_len,
        bool page_hash_equal, bool page_compare_equal, uint64_t lookup_ns);
void aot_perf_record_register_activate_tb(uint32_t aot_mode, uint32_t tb_count,
        uint32_t registered_activated_count, uint64_t register_activate_ns);

/* Save/load helpers still shared across AOT modules. */
void print_tb_bytes(TranslationBlock *tb);
void gen_hash(void);
TranslationBlock *gen_check_tb(TranslationBlock *tb, aot_block *file_tb,
        CPUArchState *env, CPUState *cpu);
TranslationBlock *register_page(CPUState *cpu, vaddr pc, uint64_t cs_base, uint32_t flags,
        int cflags, ENV *lsenv, uint64_t phys_pc);
TranslationBlock *load_aot_tb(CPUState *cpu, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags, ENV *lsenv);
TranslationBlock *load_tu(CPUState *cpu, vaddr pc,
        uint64_t cs_base, uint32_t flags, int cflags,
        vaddr hva, uint64_t file_disp, vaddr container_page, ENV *lsenv,
        uint32_t *loaded_tb_count);
TranslationBlock *load_page_tu(CPUState *cpu, vaddr pc, uint64_t cs_base, uint32_t flags,
        int cflags, vaddr hva, uint64_t file_disp, ENV *lsenv);
TranslationBlock *load_aot_tu(CPUState *cpu, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags, ENV *lsenv);


/* sys-aot-check */
void check_tu(CPUState *cpu, aot_tu_head *tu_head, TranslationBlock **curr_tb_vector, aot_block *file_tb_vector, uintptr_t tbmini_ptr, int ir2_size);
void aot_ras_clear(TranslationBlock *tb);

/* sys-aot-reloc */
extern aot_rel *rel_table;
extern aot_rel *tu_rel_table;
extern __thread int curr_rel_entry_num;
extern uintptr_t relkind_to_fixup_addr[];

void aot_reloc_init(void);
void clear_rel_table(void);
void aot_reloc_reset_on_tb_flush(int mtcc_id);
void clear_tu_rel_table(void);
bool test_tb_reloc(aot_rel *aot_rel_table, uint32_t rel_count, uint64_t tb_pc);
void aot_do_tb_reloc(ENV *lsenv, TranslationBlock *tb, aot_rel *aot_rel_table,
        uint32_t rel_count);
void add_rel_entry(TranslationBlock *tb);
void add_tu_rel_entry(TranslationBlock *tb);
void add_curr_rel_entry(TranslationBlock *tb, aot_rel_kind kind, int tc_offset,
                  uint8_t rel_slots_num, uintptr_t x86_rip_offset,
                  uintptr_t extra_addend);
#endif
