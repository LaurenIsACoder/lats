/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _IMM_CACHE_H_
#define _IMM_CACHE_H_

#include "ir1.h"
#include "ir2.h"
#include "latx-options.h"

#define LATXS_IMM_CACHE_ENABLE
#define LATXS_IMM_CACHE_DEBUG

#define CACHE_DEFAULT_CAPACITY 4
#define CACHE_MAX_CAPACITY 300

extern FILE *log_file;
extern FILE *trace_file;

#define imm_log(FMT, ...)                                                                          \
    do {                                                                                           \
        if (unlikely(option_imm_debug)) {                                                          \
            fprintf(log_file, FMT, ##__VA_ARGS__);                                                 \
        }                                                                                          \
    } while (0)

#define imm_trace(FMT, ...)                                                                        \
    do {                                                                                           \
        if (unlikely(option_imm_debug)) {                                                          \
            if (unlikely(trace_file == NULL))                                                      \
                trace_file_init();                                                                 \
            if (likely(trace_file != NULL))                                                        \
                fprintf(trace_file, FMT, ##__VA_ARGS__);                                           \
        }                                                                                          \
    } while (0)

typedef struct
{
    /* addition data */
    bool free;
    int use_count;
    // last used imm index in tb
    int lifecycle;
    // 3-6
    int itemp_index;
    bool pre_cache;

    /* bucket data */
    /*base(-100)indicates rip*/
    int base;
    int index;
    int scale;

    /**
     * record the ir1 index when cached
     * later compare with last updated ir1 index to check whether base and
     * index reg changed
     */
    int curr_ir1_index;

    /* unscaned offset use 'offset' */
    int64 offset;
    /**
     * offset scaned use 'avg_offset'
     * use avg_offset to maxmize the offset range which si12 can reach
     */
    int64 avg_offset;
    int64 min_offset;
    int64 max_offset;
} IMM_CACHE_BUCKET;

/**
 * imm format:
 *   (offset can be 0)
 *   regular imm = offset
 *   rip imm = rip + offset
 *   complex mem = base + index * scale + offset
 *     compare base
 *     compare index and scale
 *     compare offset
 *     compare final mem
 * imm diff cache:
 * successor inst can use the diff to optimize the imm
 */
typedef struct
{
    IMM_CACHE_BUCKET *bucket;

    int cache_count;
    int free_count;
    /* record curr imm index, lifecycle < curr_imm_index means cache die */
    int curr_imm_index;
    /*record ir1_index to be translated*/
    int curr_ir1_index;
    int curr_ir2_index;
    // if optimized_ir2, print if log
    bool optimized_ir2;
    long curr_pc;
    /**
     * true alloc fail and use ra itemp
     * 0 use cached itemp
     * if we use cached itemp, ra_free_temp should skip
     */
    bool itemp_allocated;
    /**
    * used for update cache value when call by latxs_convert_mem_opnd_no_offset
     */
    int curr_opt_cache_id;
    bool opt_take_effect;
    bool tb_start;
    /**
     * for complex imm, we need to compare whether base/index reg has been
     * changed since last cached. record the latest ir1_reg(rax,etc.) updated
     * index compare the last updated index with the index of base/index reg
     * when caching
     * 1-16 eax_index - r15_index defined in reg_map.h
     */
    int ir1_reg_last_updated_index[16];
} IMM_CACHE;

/* imm_cache response */
typedef struct
{
    long diff;
    int64 offset;
    int itemp_index;
    int cache_id;
    bool pre_cache;
    bool cached;
} IMM_CACHE_RES;

void imm_cache_init(int capacity);
void imm_cache_fill_bucket(int cache_id, int base, int index, int scale, int64 offset);
void imm_cache_sort(void);
void imm_cache_replace_bucket(int cache_id, int base, int index, int scale, int64 offset);
void imm_cache_precache_put(int base, int index, int scale, int64 offset);

void imm_cache_finish_precache(void);
int imm_cache_get(int base, int index, int scale, int64 offset);
bool imm_cache_can_use(int bucket_id, int base, int index);
bool imm_cache_check_base_index_has_changed(int bucket_id, int base_reg_num, int index_reg_num);

IMM_CACHE_RES imm_cache_allocate(int base, int index, int scale, int64 offset);

long imm_cache_diff(int cache_id, int64 new_offset);
void imm_cache_update_by_offset(int cache_id, int64 new_offset);
void imm_cache_update_by_diff(int cache_id, long diff);
void imm_cache_addi_update(long mem_offset);

int imm_cache_put(int base, int index, int scale, int64 offset);

void imm_cache_update_ir1_usage(IR1_INST *pir1, int curr_ir1_index);
// void imm_cache_update_all_reg_ir1_usage(IR1_INST *pir1, int curr_ir1_index);
// bool imm_cache_is_cached_at(int cache_index);
// int imm_cache_la_reg_num_at(int cache_index);

//=======validation function=======
bool imm_cache_skip_curr_ir1(void);
bool imm_cache_skip_for_instptr(void);
//======cache_free======
void imm_cache_free_itemp(int itemp_index);
void imm_cache_free_dead_cache(void);
void imm_cache_free(int id);
void imm_cache_free_all(void);
bool imm_cache_is_imm_itemp(int itemp_num);
bool imm_cache_is_using_temp(int itemp_index);

//=========debug========
void imm_cache_print_cache(void);
void imm_cache_print_bucket(IMM_CACHE_BUCKET *bucket, int index);
void imm_cache_print_ir2(IR2_INST *ir2, int index);
void imm_cache_print_tb_ir1(TranslationBlock *tb);
void imm_cache_print_ir1(IR1_INST *pir1);
void imm_cache_print_tr_ir2(void);
void imm_log_tb_pc(uint64_t pc);
bool dynamic_option_imm_cache(void);
// void imm_cache_set_dynamic_option(bool option_value);
void imm_cache_set_dynamic_option(bool option_value, const char *message);

//=========latxs init======

void latxs_imm_init(TranslationBlock *tb);
bool can_do_imm(void);
void trace_file_init(void);
// extern __thread int imm_debug;
#endif
