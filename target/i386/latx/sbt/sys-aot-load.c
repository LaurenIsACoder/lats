/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sys-aot.h"
#include "sys-aot-internal.h"
#include "sys-aot-common.h"
#include "sys-aot-format.h"
#include "qemu/rcu.h"
#include "accel/tcg//tb-hash.h"
/* #include "exec/helper-head.h" */
/* #include "helper.h" */
#include "tcg/tcg.h"
#include "tcg/tcg-ldst.h"
#include "translate.h"
#include "latx-helper.h"
#include <math.h>
#include "latx-static-codes.h"
#include <pthread.h>
#include "include/tcg/tcg-split-tb.h"
#include "latx-options.h"
#include "latx-misc.h"
#include <syscall.h>
#include "include/qemu/cacheinfo.h"
#include "accel/tcg/internal.h"
#include "accel/tcg/tb-context.h"
#include "include/latx-hack.h"
#include "include/exec/ram_addr.h"
#include "include/tcg/insn-start-words.h"
#include "file_ctx.h"
#include "qemu/typedefs.h"
#include "include/tcg/fast-itlb.h"
#include "include/crypto/aes.h"
#include "latx-ras.h"
#include "latx-tu.h"
#include "include/tcg/tcg.h"
#include "tcg/tcg-internal.h"
#include "include/qemu/cacheflush.h"
#include "include/exec/cputlb.h"
#include "include/hamt-ptw.h"
#include "accel/tcg/perf.h"
#include "latx-sys-mmio.h"
#include "include/exec/tb-flush.h"
#include "latx/offline/latx_offline_mode.h"
#include <limits.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef CONFIG_SOFTMMU
static void *qemu_ram_addr_to_host(uint64_t addr)
{
    fprintf(stderr, "%s:%d user mode not support aot!\n", __func__, __LINE__);
    exit(-1);
    return NULL;
}
#endif

#if DEBUG_AOT
static uint64_t t1, t2;
static int mis_tb;
uint64_t load_aot_time;
uint64_t recover_time;
static uint64_t cmp_time;
static uint64_t hash_time;
static uint64_t for_time;
static uint64_t read_time;
static uint64_t __attribute__((unused)) load_page_tb_time;
static uint64_t register_page_tb_time;
uint64_t load_page_count;
static uint64_t __attribute__((unused)) aot_gen_code_time;
static uint64_t __attribute__((unused)) insert_time;
static uint64_t __attribute__((unused)) time2;

static uint64_t load_tu_time, load_page_tu_time;

static uint32_t mis_page_count;

int load_core_tb_count, load_user_tb_count;
static int register_tb_count;
#endif

#define AOT_PERF_SCHEMA_VERSION "1.0"
#define AOT_PERF_TICKS_PER_SECOND 100000000.0

typedef struct AotPerfCounters {
    uint64_t mmap_ns_core;
    uint64_t mmap_ns_user;
    uint64_t index_build_ns_core;
    uint64_t index_build_ns_user;
    uint64_t loaded_pages_total;
    uint64_t loaded_tb_core;
    uint64_t loaded_tb_user;
    uint64_t lookup_count;
    uint64_t lookup_hit_count;
    uint64_t lookup_reject_count;
    uint64_t lookup_probe_total;
    uint64_t lookup_probe_max;
    uint64_t page_compare_match_count;
    uint64_t page_compare_reject_count;
    uint64_t runtime_lookup_ns;
    uint64_t register_activate_page_count;
    uint64_t register_activate_tb_count;
    uint64_t register_activate_ns;
} AotPerfCounters;

typedef struct AOTLoadedFileIndex {
    aot_tail *tail;
    page_info_array_t *info_array;
    uint32_t page_num;
    uint32_t hash_size;
    uint64_t *hash_table;
    uint64_t bucket_collision_count;
    uint32_t linear_probe_max;
} AOTLoadedFileIndex;

typedef struct AOTLoadIndexSelection {
    AOTPublishedFile *file;
    void *tb_ptr;
    uint64_t *hash_table;
    uint32_t hash_size;
    uint32_t file_generation;
    uint32_t aot_mode;
    bool use_tu;
} AOTLoadIndexSelection;

static uint32_t next_aot_file_generation;

static uint32_t aot_alloc_file_generation(void)
{
    uint32_t generation;

    do {
        generation = (qatomic_fetch_inc(&next_aot_file_generation) + 1) &
            INT32_MAX;
    } while (generation == 0);
    return generation;
}

static int aot_perf_initialized;
static int aot_perf_enabled_flag;
static int aot_perf_summary_emitted;
static const char *aot_perf_run_id = "unknown";
static const char *aot_perf_scenario_id = "unknown";
static const char *aot_perf_mode = "unknown";
static int aot_perf_repeat_index;
static AotPerfCounters aot_perf_counters;

static __thread tb_page_addr_t *aot_tu_locked_pages;
static __thread int aot_tu_locked_pages_num;

#ifdef CONFIG_LATX_TU
typedef struct AOTTUPageDependency {
    vaddr vp;
    tb_page_addr_t pp;
    int64_t pc_delta;
    void *host_page;
} AOTTUPageDependency;

static __thread AOTTUPageDependency aot_tu_dependencies[AOT_TU_MAX_PAGES];
static __thread int aot_tu_dependencies_num;
#endif

static void aot_unlock_load_pages(tb_page_addr_t fallback_page)
{
    if (aot_tu_locked_pages_num) {
        for (int i = aot_tu_locked_pages_num - 1; i >= 0; i--) {
            tb_unlock_page0(aot_tu_locked_pages[i]);
        }
        g_free(aot_tu_locked_pages);
        aot_tu_locked_pages = NULL;
        aot_tu_locked_pages_num = 0;
#ifdef CONFIG_LATX_TU
        aot_tu_dependencies_num = 0;
#endif
    } else if (fallback_page != (tb_page_addr_t)-1) {
        tb_unlock_page0(fallback_page);
    }
}

static const char *aot_perf_kind_name(uint32_t aot_mode)
{
    if (aot_mode == AOT_CORE) {
        return "core";
    }
    if (aot_mode == AOT_USER) {
        return "user";
    }
    if (aot_mode == AOT_TU) {
        return "tu";
    }
    return "unknown";
}

static uint64_t aot_perf_read_u64(uint64_t *value)
{
    return __atomic_load_n(value, __ATOMIC_RELAXED);
}

static void aot_perf_add_u64(uint64_t *value, uint64_t delta)
{
    __sync_fetch_and_add(value, delta);
}

static void aot_perf_max_u64(uint64_t *value, uint64_t candidate)
{
    uint64_t old = aot_perf_read_u64(value);
    while (candidate > old) {
        if (__sync_bool_compare_and_swap(value, old, candidate)) {
            return;
        }
        old = aot_perf_read_u64(value);
    }
}

static double aot_perf_seconds(uint64_t ticks)
{
    return (double)ticks / AOT_PERF_TICKS_PER_SECOND;
}

static uint64_t aot_perf_timestamp_ns(void)
{
    struct timespec ts;
    struct timeval tv;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL +
            (uint64_t)ts.tv_nsec;
    }
    if (gettimeofday(&tv, NULL) == 0) {
        return (uint64_t)tv.tv_sec * 1000000000ULL +
            (uint64_t)tv.tv_usec * 1000ULL;
    }
    return 0;
}

static const char *aot_perf_env_or_unknown(const char *name)
{
    const char *value = getenv(name);

    return value && value[0] ? value : "unknown";
}

static int aot_perf_parse_int_env(const char *name, int fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    long parsed;

    if (!value || !value[0]) {
        return fallback;
    }

    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
        return fallback;
    }

    return (int)parsed;
}

static void aot_perf_json_string(const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");

    fputc('"', stderr);
    for (; *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", stderr);
            break;
        case '\\':
            fputs("\\\\", stderr);
            break;
        case '\b':
            fputs("\\b", stderr);
            break;
        case '\f':
            fputs("\\f", stderr);
            break;
        case '\n':
            fputs("\\n", stderr);
            break;
        case '\r':
            fputs("\\r", stderr);
            break;
        case '\t':
            fputs("\\t", stderr);
            break;
        default:
            if (*p < 0x20) {
                fprintf(stderr, "\\u%04x", *p);
            } else {
                fputc(*p, stderr);
            }
            break;
        }
    }
    fputc('"', stderr);
}

static void aot_perf_emit_event_begin(const char *event)
{
    fprintf(stderr, "[AOT-PERF] {\"event\":");
    aot_perf_json_string(event);
    fprintf(stderr, ",\"schema_version\":");
    aot_perf_json_string(AOT_PERF_SCHEMA_VERSION);
    fprintf(stderr, ",\"run_id\":");
    aot_perf_json_string(aot_perf_run_id);
    fprintf(stderr, ",\"scenario_id\":");
    aot_perf_json_string(aot_perf_scenario_id);
    fprintf(stderr, ",\"repeat_index\":%d,\"aot_mode\":",
            aot_perf_repeat_index);
    aot_perf_json_string(aot_perf_mode);
    fprintf(stderr, ",\"pid\":%d,\"timestamp_ns\":%llu",
            (int)getpid(), (unsigned long long)aot_perf_timestamp_ns());
}

void aot_perf_init_from_env(void)
{
    const char *enabled;

    if (aot_perf_initialized) {
        return;
    }

    enabled = getenv("LATS_AOT_PERF");
    aot_perf_enabled_flag = enabled && enabled[0] == '1';
    aot_perf_run_id = aot_perf_env_or_unknown("LATS_AOT_PERF_RUN_ID");
    aot_perf_scenario_id =
        aot_perf_env_or_unknown("LATS_AOT_PERF_SCENARIO_ID");
    aot_perf_mode = aot_perf_env_or_unknown("LATS_AOT_PERF_MODE");
    aot_perf_repeat_index =
        aot_perf_parse_int_env("LATS_AOT_PERF_REPEAT_INDEX", 0);
    aot_perf_initialized = 1;
}

bool aot_perf_enabled(void)
{
    if (!aot_perf_initialized) {
        aot_perf_init_from_env();
    }
    return aot_perf_enabled_flag;
}

uint64_t aot_perf_now_ns(void)
{
#if DEBUG_AOT
    return get_tsc();
#else
    return 0;
#endif
}

void aot_perf_emit_run_context(int aot_enabled, int aot_load_enabled,
        int aot_save_enabled)
{
    if (!aot_perf_enabled()) {
        return;
    }
    aot_perf_emit_event_begin("run_context");
    fprintf(stderr,
            ",\"aot_enabled\":%d,\"aot_load_enabled\":%d,"
            "\"aot_save_enabled\":%d}\n",
            aot_enabled, aot_load_enabled, aot_save_enabled);
}

void aot_perf_record_load_file(uint32_t aot_mode, const char *file_name,
        uint64_t file_size, uint32_t page_count, uint32_t tb_count,
        bool integrity_ok, uint64_t mmap_ns)
{
    if (!aot_perf_enabled()) {
        return;
    }

    if (aot_mode == AOT_CORE) {
        aot_perf_add_u64(&aot_perf_counters.mmap_ns_core, mmap_ns);
        aot_perf_add_u64(&aot_perf_counters.loaded_tb_core, tb_count);
    } else if (aot_mode == AOT_USER) {
        aot_perf_add_u64(&aot_perf_counters.mmap_ns_user, mmap_ns);
        aot_perf_add_u64(&aot_perf_counters.loaded_tb_user, tb_count);
    }
    aot_perf_add_u64(&aot_perf_counters.loaded_pages_total, page_count);

    aot_perf_emit_event_begin("load_file");
    fprintf(stderr, ",\"kind\":");
    aot_perf_json_string(aot_perf_kind_name(aot_mode));
    fprintf(stderr, ",\"file_name\":");
    aot_perf_json_string(file_name ? file_name : "");
    fprintf(stderr,
            ",\"file_size\":%llu,\"page_count\":%u,"
            "\"tb_count\":%u,\"integrity_ok\":%s,"
            "\"mmap_seconds\":%.9f}\n",
            (unsigned long long)file_size, page_count, tb_count,
            integrity_ok ? "true" : "false", aot_perf_seconds(mmap_ns));
}

void aot_perf_record_build_index(uint32_t aot_mode, uint32_t page_count,
        uint32_t hash_size, uint64_t bucket_collision_count,
        uint32_t linear_probe_max, uint64_t build_ns)
{
    if (!aot_perf_enabled()) {
        return;
    }

    if (aot_mode == AOT_CORE) {
        aot_perf_add_u64(&aot_perf_counters.index_build_ns_core, build_ns);
    } else if (aot_mode == AOT_USER) {
        aot_perf_add_u64(&aot_perf_counters.index_build_ns_user, build_ns);
    }

    aot_perf_emit_event_begin("build_index");
    fprintf(stderr, ",\"kind\":");
    aot_perf_json_string(aot_perf_kind_name(aot_mode));
    fprintf(stderr,
            ",\"page_count\":%u,\"hash_size\":%u,"
            "\"bucket_collision_count\":%llu,"
            "\"linear_probe_max\":%u,\"build_seconds\":%.9f}\n",
            page_count, hash_size,
            (unsigned long long)bucket_collision_count, linear_probe_max,
            aot_perf_seconds(build_ns));
}

void aot_perf_record_lookup(bool hit, uint32_t probe_len,
        bool page_hash_equal, bool page_compare_equal, uint64_t lookup_ns)
{
    if (!aot_perf_enabled()) {
        return;
    }

    aot_perf_add_u64(&aot_perf_counters.lookup_count, 1);
    aot_perf_add_u64(&aot_perf_counters.lookup_probe_total, probe_len);
    aot_perf_max_u64(&aot_perf_counters.lookup_probe_max, probe_len);
    aot_perf_add_u64(&aot_perf_counters.runtime_lookup_ns, lookup_ns);
    if (hit) {
        aot_perf_add_u64(&aot_perf_counters.lookup_hit_count, 1);
    } else {
        aot_perf_add_u64(&aot_perf_counters.lookup_reject_count, 1);
    }
    if (page_compare_equal) {
        aot_perf_add_u64(&aot_perf_counters.page_compare_match_count, 1);
    } else if (page_hash_equal) {
        aot_perf_add_u64(&aot_perf_counters.page_compare_reject_count, 1);
    }
}

void aot_perf_record_register_activate_tb(uint32_t aot_mode, uint32_t tb_count,
        uint32_t registered_activated_count, uint64_t register_activate_ns)
{
    (void)aot_mode;
    (void)tb_count;

    if (!aot_perf_enabled()) {
        return;
    }

    aot_perf_add_u64(&aot_perf_counters.register_activate_page_count, 1);
    aot_perf_add_u64(&aot_perf_counters.register_activate_tb_count,
            registered_activated_count);
    aot_perf_add_u64(&aot_perf_counters.register_activate_ns,
            register_activate_ns);
}

void aot_perf_emit_summary(void)
{
    uint64_t mmap_core, mmap_user, index_core, index_user;
    uint64_t lookup_ns, register_activate_ns, aot_load_ns;
    uint64_t loaded_tb_core, loaded_tb_user;

    if (!aot_perf_enabled()) {
        return;
    }
    if (__sync_lock_test_and_set(&aot_perf_summary_emitted, 1)) {
        return;
    }

    mmap_core = aot_perf_read_u64(&aot_perf_counters.mmap_ns_core);
    mmap_user = aot_perf_read_u64(&aot_perf_counters.mmap_ns_user);
    index_core = aot_perf_read_u64(&aot_perf_counters.index_build_ns_core);
    index_user = aot_perf_read_u64(&aot_perf_counters.index_build_ns_user);
    lookup_ns = aot_perf_read_u64(&aot_perf_counters.runtime_lookup_ns);
    register_activate_ns =
        aot_perf_read_u64(&aot_perf_counters.register_activate_ns);
    loaded_tb_core = aot_perf_read_u64(&aot_perf_counters.loaded_tb_core);
    loaded_tb_user = aot_perf_read_u64(&aot_perf_counters.loaded_tb_user);
    aot_load_ns = mmap_core + mmap_user + index_core + index_user +
        lookup_ns + register_activate_ns;

    aot_perf_emit_event_begin("runtime_lookup_summary");
    fprintf(stderr,
            ",\"lookup_count\":%llu,\"lookup_hit_count\":%llu,"
            "\"lookup_reject_count\":%llu,\"lookup_probe_total\":%llu,"
            "\"lookup_probe_max\":%llu,"
            "\"page_compare_match_count\":%llu,"
            "\"page_compare_reject_count\":%llu,"
            "\"runtime_lookup_seconds\":%.9f}\n",
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.lookup_count),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.lookup_hit_count),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.lookup_reject_count),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.lookup_probe_total),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.lookup_probe_max),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.page_compare_match_count),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.page_compare_reject_count),
            aot_perf_seconds(lookup_ns));
    aot_perf_emit_event_begin("register_activate_tb_summary");
    fprintf(stderr,
            ",\"register_activate_page_count\":%llu,"
            "\"register_activate_tb_count\":%llu,"
            "\"register_activate_seconds\":%.9f}\n",
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.register_activate_page_count),
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.register_activate_tb_count),
            aot_perf_seconds(register_activate_ns));
    aot_perf_emit_event_begin("summary");
    fprintf(stderr,
            ",\"loaded_pages_total\":%llu,"
            "\"loaded_tb_core\":%llu,\"loaded_tb_user\":%llu,"
            "\"loaded_tb_total\":%llu,"
            "\"aot_mmap_seconds_core\":%.9f,"
            "\"aot_mmap_seconds_user\":%.9f,"
            "\"aot_index_build_seconds_core\":%.9f,"
            "\"aot_index_build_seconds_user\":%.9f,"
            "\"aot_runtime_lookup_seconds\":%.9f,"
            "\"aot_tb_register_activate_seconds\":%.9f,"
            "\"aot_load_seconds\":%.9f}\n",
            (unsigned long long)aot_perf_read_u64(&aot_perf_counters.loaded_pages_total),
            (unsigned long long)loaded_tb_core,
            (unsigned long long)loaded_tb_user,
            (unsigned long long)(loaded_tb_core + loaded_tb_user),
            aot_perf_seconds(mmap_core),
            aot_perf_seconds(mmap_user),
            aot_perf_seconds(index_core),
            aot_perf_seconds(index_user),
            aot_perf_seconds(lookup_ns),
            aot_perf_seconds(register_activate_ns),
            aot_perf_seconds(aot_load_ns));
}

static bool aot_load_file_validate_index(void *mem_ptr, uint64_t file_size,
        AOTLoadedFileIndex *index)
{
    aot_tail *tail;
    size_t page_info_size;

    if (file_size < sizeof(aot_tail)) {
        fprintf(stderr, "[AOT] Load: invalid file size\n");
        return false;
    }

    tail = (aot_tail *)((uint8_t *)mem_ptr + file_size - sizeof(aot_tail));
    if (tail->page_num < 0) {
        fprintf(stderr, "[AOT] Load: invalid page count: %d\n",
                tail->page_num);
        return false;
    }

    index->page_num = (uint32_t)tail->page_num;
    if (!aot_page_info_size(index->page_num, &page_info_size) ||
            page_info_size > file_size - sizeof(aot_tail)) {
        fprintf(stderr, "[AOT] Load: invalid page info size\n");
        return false;
    }

    index->tail = tail;
    index->info_array =
        (page_info_array_t *)((uint8_t *)tail - page_info_size);
    if (index->info_array->page_count != index->page_num) {
        fprintf(stderr, "[AOT] Load: page count mismatch\n");
        return false;
    }

    return true;
}

static bool aot_load_file_prepare_hash(AOTLoadedFileIndex *index)
{
    uint64_t hash_target = (uint64_t)index->page_num * 2;

    if (hash_target > ((uint32_t)-1)) {
        fprintf(stderr, "[AOT] Load: invalid hash size\n");
        return false;
    }

    index->hash_size = 1;
    while ((uint64_t)index->hash_size < hash_target) {
        index->hash_size <<= 1;
    }

    index->hash_table =
        aligned_alloc(64, index->hash_size * sizeof(uint64_t));
    if (!index->hash_table) {
        fprintf(stderr, "[AOT] Load: alloc hash table failed\n");
        return false;
    }
    memset(index->hash_table, -1, index->hash_size * sizeof(uint64_t));
    return true;
}

static void aot_load_file_build_hash(AOTLoadedFileIndex *index)
{
    for (uint32_t i = 0; i < index->info_array->page_count; i++) {
        uint32_t h = index->info_array->pages[i].page_hash;
        uint32_t idx = h & (index->hash_size - 1);
        uint32_t probe_len = 0;

        while (index->hash_table[idx] != (uint64_t)-1) {
            index->bucket_collision_count++;
            probe_len++;
            idx = (idx + 1) & (index->hash_size - 1);
        }
        if (probe_len > index->linear_probe_max) {
            index->linear_probe_max = probe_len;
        }
        index->hash_table[idx] = index->info_array->pages[i].file_offset;
    }
}

AOTPublishedFile *load_aot_file(const char *file_name, uint32_t aot_mode)
{
    AOTPublishedFile *published = NULL;
    void *mem_ptr = NULL;
    uint64_t size = 0;
    bool perf_enabled = aot_perf_enabled();
    uint64_t mmap_begin = perf_enabled ? aot_perf_now_ns() : 0;
    uint64_t index_begin;
    AOTLoadedFileIndex index = { 0 };

    size_t aot_file_name_len = strlen(aot_file_path) + strlen(file_name) + 1;
    char *aot_file_name = malloc(aot_file_name_len);
    if (!aot_file_name) {
        fprintf(stderr, "[AOT] Load: alloc file name failed\n");
        return NULL;
    }
    snprintf(aot_file_name, aot_file_name_len, "%s%s", aot_file_path, file_name);
    if (access(aot_file_name, 0) < 0) {
        goto fail_no_lock;
    }

    int lockfd = -1;
    char *aot_file_lock = gen_file_lock(aot_file_name);
    if (!aot_file_lock) {
        goto fail_no_lock;
    }
    if (file_lock(aot_file_lock, &lockfd, F_RDLCK, false) < 0) {
        if (lockfd >= 0) {
            close(lockfd);
        }
        goto fail_lock;
    }

    mem_ptr = load_file_to_mem(aot_file_name, &size, aot_mode);
    if (!mem_ptr) {
        aot_perf_record_load_file(aot_mode, aot_file_name, size, 0, 0,
                false, perf_enabled ? aot_perf_now_ns() - mmap_begin : 0);
        goto fail;
    }

    if (!aot_load_file_validate_index(mem_ptr, size, &index) ||
            !aot_load_file_prepare_hash(&index)) {
        goto fail;
    }

    aot_perf_record_load_file(aot_mode, aot_file_name, size,
            index.tail->page_num, index.tail->tb_num, true,
            perf_enabled ? aot_perf_now_ns() - mmap_begin : 0);
    index_begin = perf_enabled ? aot_perf_now_ns() : 0;
    aot_load_file_build_hash(&index);
    aot_perf_record_build_index(aot_mode, index.tail->page_num,
            index.hash_size, index.bucket_collision_count,
            index.linear_probe_max,
            perf_enabled ? aot_perf_now_ns() - index_begin : 0);
    published = g_try_new0(AOTPublishedFile, 1);
    if (!published) {
        goto fail;
    }
    published->tb_ptr = mem_ptr;
    published->size = size;
    published->hash_table = index.hash_table;
    published->hash_size = index.hash_size;
    published->generation = aot_alloc_file_generation();
    index.hash_table = NULL;

    aot_verbose_log("[AOT][Load] Opened: %s, pages=%u, hash_size=%u\n",
            aot_file_name, index.page_num, index.hash_size);

    flock_set(lockfd, F_UNLCK, true);
    close(lockfd);
    free(aot_file_lock);
    free(aot_file_name);
    return published;

fail:
    flock_set(lockfd, F_UNLCK, true);
    close(lockfd);
    free(aot_file_lock);
    free(index.hash_table);
    free(published);
    if (mem_ptr) {
        munmap(mem_ptr, size);
    }
    remove(aot_file_name);
    free(aot_file_name);
    return NULL;

fail_lock:
    free(aot_file_lock);
fail_no_lock:
    free(aot_file_name);
    return NULL;
}

/* __thread bool aot_load_ok; */
static inline int recover_tb(TranslationBlock *curr_tb, aot_block *aot_tb, int *search_size,
        aot_rel *rel_table, uint32_t rel_count, void *code_highwater, ENV *lsenv, int mtcc_id)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif
    int ir2_size = aot_tb->ir2_num << 2;
    const void* code_end = curr_tb->tc.ptr + ir2_size;
    if (!option_split_search) {
        code_end += aot_tb->search_size;
    }
    if (code_end >= code_highwater) {
        return -1;
    } else {
        curr_tb->preorder_tb_offset = 0;
        curr_tb->preorder_next = NULL;
#ifdef CONFIG_LATX_TU
        curr_tb->s_data->tu_cold_exit_offset[0] = TU_COLD_EXIT_INVALID;
        curr_tb->s_data->tu_cold_exit_offset[1] = TU_COLD_EXIT_INVALID;
        curr_tb->jit_tucheck_external[0] = NULL;
        curr_tb->jit_tucheck_external[1] = NULL;
        curr_tb->ras_dest = (uintptr_t)NULL;
#endif
        curr_tb->page_next[0] = 0;
        curr_tb->page_next[1] = 0;
        curr_tb->jmp_inst_hold[0] = 0;
        curr_tb->jmp_inst_hold[1] = 0;

        curr_tb->flags = aot_tb->flags;
        curr_tb->cflags = aot_tb->cflags;
        curr_tb->icount = aot_tb->icount;
        curr_tb->size = aot_tb->size;
        curr_tb->ir2_num = aot_tb->ir2_num;
        curr_tb->jmp_reset_offset[0] = aot_tb->jmp_reset_offset[0];
        curr_tb->jmp_reset_offset[1] = aot_tb->jmp_reset_offset[1];
        curr_tb->jmp_insn_offset[0]  = aot_tb->jmp_insn_offset[0];
        curr_tb->jmp_insn_offset[1]  = aot_tb->jmp_insn_offset[1];
        curr_tb->jmp_target_addr[0] = aot_tb->jmp_target_addr[0];
        curr_tb->jmp_target_addr[1] = aot_tb->jmp_target_addr[1];
        curr_tb->next_pc[0] = aot_tb->next_pc[0] + ((int64_t)curr_tb->pc - (int64_t)aot_tb->pc);
        curr_tb->next_pc[1] = aot_tb->next_pc[1] + ((int64_t)curr_tb->pc - (int64_t)aot_tb->pc);
        curr_tb->link_opt[0] = aot_tb->link_opt[0];
        curr_tb->link_opt[1] = aot_tb->link_opt[1];
        curr_tb->int_opt[0] = aot_tb->int_opt[0];
        curr_tb->int_opt[1] = aot_tb->int_opt[1];
        /* curr_tb->preorder_tb_offset = aot_tb->preorder_tb_offset; */
        curr_tb->inst_ptr_offset = aot_tb->inst_ptr_offset;
        curr_tb->label_pos_nocheck = aot_tb->label_pos_nocheck;
        curr_tb->misc_flags = aot_tb->misc_flags;
        /* curr_tb->page_next[0] = aot_tb->page_next[0]; */
        /* curr_tb->page_next[1] = aot_tb->page_next[1]; */
        curr_tb->jmp_cp_data[0] = aot_tb->jmp_cp_data[0];
        curr_tb->jmp_cp_data[1] = aot_tb->jmp_cp_data[1];
        /* curr_tb->jmp_inst_hold[0] = aot_tb->jmp_inst_hold[0]; */
        /* curr_tb->jmp_inst_hold[1] = aot_tb->jmp_inst_hold[1]; */

        void* aot_ir2_code = (void *)((uintptr_t)aot_tb + sizeof(aot_block));
        if (option_split_search) {
            memcpy((void*)curr_tb->tc.ptr, aot_ir2_code, ir2_size);
            if (!region_alloc_search(curr_tb, mtcc_id)) {
                return -1;
            }
            memcpy((void*)curr_tb->search_ptr, aot_ir2_code + ir2_size, aot_tb->icount << 2);
            *search_size = 0;
        } else {
            memcpy((void*)curr_tb->tc.ptr, aot_ir2_code, ir2_size + aot_tb->search_size);
            *search_size = aot_tb->search_size;
            curr_tb->search_size = aot_tb->search_size;
        }
        aot_do_tb_reloc(lsenv, curr_tb, rel_table, rel_count);

#if DEBUG_AOT
        if (option_aot_debug) {
            fprintf(stderr, "aot_ir2_code point %p\n", aot_ir2_code);
            fprintf(stderr, "tb pc %lx %lx\n", curr_tb->pc, aot_tb->pc);
            fprintf(stderr, "ir2_num %d %d\n", curr_tb->ir2_num, aot_tb->ir2_num);
            uint32_t *buf0 = (uint32_t *)curr_tb->tc.ptr;
            /* uint32_t *buf0 = (uint32_t *)aot_ir2_code; */
            for (int i = 0; i < aot_tb->ir2_num; i++) {
                fprintf(stderr, "i %d %x\n", i, buf0[i]);
            }
            /* aot_load_ok = true; */
        }

        recover_time += get_tsc() - begin;
#endif

        return ir2_size;
    }
    return 0;
}

static uint64_t find_page_with_hash(ADDR hva, uint32_t h,
        uint64_t perf_begin)
{
    bool perf_enabled = aot_perf_enabled();
    uint32_t probe_len = 0;
    bool page_hash_equal = false;
    bool page_compare_equal = false;
    int idx = (h) & (aot_hash_size - 1);
    int i = 0;
#if DEBUG_AOT
    uint64_t for_begin = get_tsc();
#endif
    for (i = 0; i < aot_hash_size; i++)
    {
        uint64_t file_disp = hash_table[idx];
        probe_len++;
        if (file_disp != (uint64_t)-1) {
#if DEBUG_AOT
            uint64_t read_begin = get_tsc();
#endif
            uint64_t file_addr = (uint64_t)in_tb_ptr + file_disp;
            page_head *ph = (page_head *)file_addr;
            void *guest_code = (void *)(file_addr+ sizeof(page_head));
#if DEBUG_AOT
            read_time += get_tsc() - read_begin;
            uint64_t cmp_begin = get_tsc();
#endif
            if (ph->page_magic == AOT_PAGE_MAGIC && ph->page_hash == h) {
                page_hash_equal = true;
                if (memcmp((void*)hva, guest_code, TARGET_PAGE_SIZE) == 0) {
                    page_compare_equal = true;
#if DEBUG_AOT
                    cmp_time += get_tsc() - cmp_begin;
                    load_page_count++;
                    for_time += get_tsc() - for_begin;
#endif
                    if (perf_enabled) {
                        aot_perf_record_lookup(true, probe_len, page_hash_equal,
                                page_compare_equal,
                                aot_perf_now_ns() - perf_begin);
                    }
                    return file_addr;
                }
            } else if (ph->page_magic != AOT_PAGE_MAGIC) {
                fprintf(stderr, "find page bad page_magic %x\n", ph->page_magic);
                exit(0);
            }
#if DEBUG_AOT
            cmp_time += get_tsc() - cmp_begin;
#endif
        } else {
            break;
        }
        idx = (idx + 1) & (aot_hash_size - 1);
    }
#if DEBUG_AOT
    for_time += get_tsc() - for_begin;
    mis_page_count++;
#endif

    if (perf_enabled) {
        aot_perf_record_lookup(false, probe_len, page_hash_equal,
                page_compare_equal, aot_perf_now_ns() - perf_begin);
    }
    return 0;
}

uint64_t find_page(ADDR hva)
{
    bool perf_enabled = aot_perf_enabled();
    uint64_t perf_begin = perf_enabled ? aot_perf_now_ns() : 0;
#if DEBUG_AOT
    uint64_t hash_begin = get_tsc();
#endif
    uint32_t h = get_page_hash((uint32_t *)(hva & TARGET_PAGE_MASK));
#if DEBUG_AOT
    hash_time += get_tsc() - hash_begin;
#endif

    return find_page_with_hash(hva, h, perf_begin);
}

#ifdef CONFIG_LATX_TU
static uint64_t find_aot_tu_page(ADDR hva, tb_page_addr_t phys_page)
{
    AOTTUPageCache cache;
    bool cache_valid = get_page_aot_tu_cache(phys_page, &cache);

    if (cache_valid && cache.file_generation == aot_file_generation) {
        if (cache.file_offset == UINT64_MAX) {
            return 0;
        }
        return (uint64_t)in_tb_ptr + cache.file_offset;
    }

    uint64_t file_addr = find_page(hva);
    uint64_t file_offset = file_addr ?
        file_addr - (uint64_t)in_tb_ptr : UINT64_MAX;
    set_page_aot_tu_cache(phys_page, aot_file_generation, file_offset);
    return file_addr;
}
#endif

#ifdef CONFIG_LATX_TU
/* A candidate page is not necessarily the guest's current fetch address. */
static tb_page_addr_t aot_probe_code_page(CPUArchState *env, vaddr pc,
                                         void **hostp)
{
    int saved_error_code = env->error_code;
    tb_page_addr_t pp = get_page_addr_code_hostp_noexit(env, pc, hostp);

    /* A failed x86 probe writes error_code even without raising a fault. */
    env->error_code = saved_error_code;
    return pp;
}

static uint64_t aot_find_tu_page_record(CPUState *cpu, vaddr page_pc,
        ADDR *page_hva, tb_page_addr_t *page_phys)
{
    CPUArchState *env = cpu->env_ptr;

    for (int retry = 0; retry < AOT_TU_MAX_PAGES; retry++) {
        void *host_page = NULL;
        tb_page_addr_t phys = aot_probe_code_page(env, page_pc,
                &host_page);

        if (phys == (tb_page_addr_t)-1 ||
                !aot_host_pc_in_main_ram(host_page)) {
            return 0;
        }
        phys &= TARGET_PAGE_MASK;
        tb_lock_page0(phys);

        void *locked_host_page = NULL;
        tb_page_addr_t locked_phys = aot_probe_code_page(env,
                page_pc, &locked_host_page);
        if (locked_phys == (tb_page_addr_t)-1 ||
                (locked_phys & TARGET_PAGE_MASK) != phys ||
                !aot_host_pc_in_main_ram(locked_host_page)) {
            tb_unlock_page0(phys);
            continue;
        }

#ifdef CONFIG_SOFTMMU
#ifdef CONFIG_LATX_HACK_MEM
        if (!hamt_ptwdata_smc_page_is_protected(phys)) {
            tlb_protect_code(phys);
        }
#else
        tlb_protect_code(phys);
#endif
#endif
        uint64_t file_addr = find_aot_tu_page((ADDR)locked_host_page,
                phys >> TARGET_PAGE_BITS);
        tb_unlock_page0(phys);
        if (page_hva) {
            *page_hva = (ADDR)locked_host_page;
        }
        if (page_phys) {
            *page_phys = phys;
        }
        return file_addr;
    }
    return 0;
}
#endif

static TranslationBlock *activate_tb(TranslationBlock *tb, CPUState *cpu,
        ENV *lsenv)
{

    if (!tb || !(tb->cflags & CF_NO_ACTIVE)) {
        fprintf(stderr, "reactivate tb %lx tb->cflags %x\n", tb->pc, tb->cflags);
        /* exit(0); */
        return NULL;
    }

    int search_size;
    CPUArchState *env = cpu->env_ptr;
    uint32_t mtcc_id = curr_mtcc(env);
    tcg_ctx = tcg_ctx_mtcc[mtcc_id];
    tcg_insn_unit *gen_code_buf = tcg_ctx->code_gen_ptr;

#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    /* set TBMini */
    if (qemu_tcg_mode_2_lat()) {
        uint64_t *tbm = (uint64_t *)gen_code_buf;
        tbmini_set_pointer(tbm, (uint64_t)tb);
        gen_code_buf = (tcg_insn_unit *)((uintptr_t)gen_code_buf + sizeof(struct TBMini));
    }
#endif

    uintptr_t file_disp = (uintptr_t)tb->tc.ptr;
    if (file_disp < 0x1000 || *(uint32_t *)file_disp != AOT_TB_MAGIC) {
        uint64_t phys_pc = tb_page_addr0(tb);
        ADDR hva = (ADDR)qemu_ram_addr_to_host(phys_pc & TARGET_PAGE_MASK);
        uint64_t page_addr = find_page(hva);
        file_disp = 0;
        if (page_addr) {
            page_head *ph = (page_head *)page_addr;
            uintptr_t curr = page_addr + sizeof(page_head) + TARGET_PAGE_SIZE;
            uint32_t n = 0;
            while (n < ph->tb_count) {
                aot_block *ab = (aot_block *)(curr + 16);
                if (ab->pc == tb->pc && ab->flags == tb->flags
                        && (ab->cflags & ~CF_INVALID) == (tb->cflags & ~(CF_NO_ACTIVE | CF_INVALID))) {
                    file_disp = curr;
                    break;
                }
                curr += *(uint32_t *)(curr + 4);
                n++;
            }
        }
        if (!file_disp) return NULL;
    }
    tb->tc.ptr = (void *)tcg_splitwx_to_rx(gen_code_buf);

    uint32_t magic = *(uint32_t *)file_disp;
    if (magic != AOT_TB_MAGIC) {
        fprintf(stderr, "error tb magic %x in activate_tb tid %ld\n", magic, syscall(SYS_gettid));
        _exit(0);
    }

    tb->misc_flags &= ~MF_FLAGS_NOEFLAGS;
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
    if (tb->flags == cpu_get_tb_cpu_state_flags_no_eflags(env)) {
        add_misc_flags(tb, MF_FLAGS_NOEFLAGS);
    }
#endif
    set_mask_to_miscflags(tb, curr_mtcc(env),
            MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT);

    uint32_t all_tb_size = *(uint32_t *)(file_disp + 8);
    uint32_t rel_count = *(uint32_t *)(file_disp + 12);
    aot_block *file_tb = (aot_block *)(file_disp + 16);
    aot_rel *curr_tb_rel_table =
        (aot_rel *)((void *)file_tb + all_tb_size);

    int gen_code_size = tb->ir2_num << 2;

    void* aot_ir2_code = (void *)((uintptr_t)file_tb + sizeof(aot_block));

    int copy_size = gen_code_size;
    if (!option_split_search) {
        search_size = tb->search_size;
        copy_size += search_size;
    } else {
        search_size = 0;
        if (!region_alloc_search(tb, mtcc_id)) {
            fprintf(stderr, "region_alloc_search fail \n");
            exit(0);
            return NULL;
        }
        memcpy((void *)tb->search_ptr,
                aot_ir2_code + (tb->ir2_num << 2), tb->icount << 2);
    }

    if (option_pushpop) {
        if (!region_alloc_push_pop_data(tb, mtcc_id, tb->icount)) {
            return NULL;
        }
        memcpy(tb_opt_ptr(tb), aot_ir2_code + (tb->ir2_num << 2) + (tb->icount << 2),
                tb->icount << 2);
    }

    if (tb->tc.ptr + copy_size >= tcg_ctx->code_gen_highwater) {
        return NULL;
    }

    memcpy((void*)tb->tc.ptr, aot_ir2_code, copy_size);
    aot_do_tb_reloc(lsenv, tb, curr_tb_rel_table, rel_count);

    tb->preorder_tb_offset = 0;
    tb->preorder_next = NULL;
#ifdef CONFIG_LATX_TU
    tb->ras_dest = (uintptr_t)NULL;
#endif
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
    if (tb->flags == cpu_get_tb_cpu_state_flags_no_eflags(env)) {
        add_misc_flags(tb, MF_FLAGS_NOEFLAGS);
    }
#endif
    /* set_mask_to_miscflags(tb, curr_mtcc(env), */
    /*         MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT); */
    /* remove_misc_flags(tb, MF_LINK0_MASK | MF_LINK1_MASK | MF_LINK_STATUS); */
    if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
        if (qemu_tcg_mode_2_lat()) {
            latx_tb_reset_jmp_target(tb, 0);
        } else {
            tb_reset_jump(tb, 0);
        }
    }
    if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
        if (qemu_tcg_mode_2_lat()) {
            latx_tb_reset_jmp_target(tb, 1);
        } else {
            tb_reset_jump(tb, 1);
        }
    }
    if (tb) {
        /* 3. flush icache range */
        ADDR s = (ADDR)tb->tc.ptr;
        ADDR e = s + gen_code_size;
        flush_idcache_range((uintptr_t)s, (uintptr_t)s, (uintptr_t)e);
    }


#if defined(CONFIG_LATX_TCG_MODE) || defined(CONFIG_TCG_REGION_TREE_ENABLE)
    tb->tc.size = gen_code_size;
#endif

    /*
     * For CF_PCREL, attribute all executions of the generated code
     * to its first mapping.
     */
    /* perf_report_code(tb->pc, tb, tcg_splitwx_to_rx(gen_code_buf)); */

    uintptr_t sptr = (uintptr_t)gen_code_buf + gen_code_size;
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    if (qemu_tcg_mode_2_lat()) {
        sptr = sptr + sizeof(struct TBMini);
    }
#endif
    qatomic_set(&tcg_ctx->code_gen_ptr, (void *)
        ROUND_UP(sptr + search_size, CODE_GEN_ALIGN));

    if (option_soft_ras && tb->inst_ptr_offset &&
            ((tb->pc & TARGET_PAGE_MASK) ==
             (tb->s_data->x86_next_pc & TARGET_PAGE_MASK))) {
        TranslationBlock *next_tb = aot_tb_lookup(cpu,
                tb->s_data->x86_next_pc, tb->cs_base,
                tb->flags, tb->cflags | CF_INVALID | CF_NO_ACTIVE);
        if (next_tb) {
            next_tb = activate_tb(next_tb, cpu, lsenv);
        }else {
            next_tb = tb_lookup(cpu, tb->s_data->x86_next_pc, tb->cs_base,
                    tb->flags, tb->cflags & ~(CF_INVALID | CF_NO_ACTIVE));
        }
        if (next_tb) {
            if (!latxs_ras_link(tb, next_tb)) {
                latxs_ras_unlink(tb);
            }
        }
    }

    aot_tb_relink_page(tb);

#if DEBUG_AOT
    load_core_tb_count++;
#endif
    return tb;
}

inline int recover_tb_head(TranslationBlock *curr_tb, aot_block *aot_tb)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif

    curr_tb->bool_flags |= IS_AOT_TB;
    curr_tb->flags = aot_tb->flags;
    curr_tb->cflags = aot_tb->cflags;
    if (!option_split_search) {
        curr_tb->search_size = aot_tb->search_size;
    }
    curr_tb->preorder_tb_offset = 0;
    curr_tb->preorder_next = NULL;
#ifdef CONFIG_LATX_TU
    curr_tb->s_data->tu_cold_exit_offset[0] = TU_COLD_EXIT_INVALID;
    curr_tb->s_data->tu_cold_exit_offset[1] = TU_COLD_EXIT_INVALID;
    curr_tb->jit_tucheck_external[0] = NULL;
    curr_tb->jit_tucheck_external[1] = NULL;
    curr_tb->ras_dest = (uintptr_t)NULL;
#endif
    curr_tb->page_next[0] = 0;
    curr_tb->page_next[1] = 0;
    curr_tb->jmp_inst_hold[0] = 0;
    curr_tb->jmp_inst_hold[1] = 0;
    curr_tb->signal_unlink[0] = 0;
    curr_tb->signal_unlink[1] = 0;

    curr_tb->icount = aot_tb->icount;
    curr_tb->size = aot_tb->size;
    curr_tb->ir2_num = aot_tb->ir2_num;
    curr_tb->jmp_reset_offset[0] = aot_tb->jmp_reset_offset[0];
    curr_tb->jmp_reset_offset[1] = aot_tb->jmp_reset_offset[1];
    curr_tb->jmp_insn_offset[0]  = aot_tb->jmp_insn_offset[0];
    curr_tb->jmp_insn_offset[1]  = aot_tb->jmp_insn_offset[1];
    curr_tb->jmp_target_addr[0] = aot_tb->jmp_target_addr[0];
    curr_tb->jmp_target_addr[1] = aot_tb->jmp_target_addr[1];
    curr_tb->link_opt[0] = aot_tb->link_opt[0];
    curr_tb->link_opt[1] = aot_tb->link_opt[1];
    curr_tb->int_opt[0] = aot_tb->int_opt[0];
    curr_tb->int_opt[1] = aot_tb->int_opt[1];
    /* curr_tb->preorder_tb_offset = aot_tb->preorder_tb_offset; */
    curr_tb->inst_ptr_offset = aot_tb->inst_ptr_offset;
    curr_tb->label_pos_nocheck = aot_tb->label_pos_nocheck;
    curr_tb->misc_flags = aot_tb->misc_flags;
    /* curr_tb->page_next[0] = aot_tb->page_next[0]; */
    /* curr_tb->page_next[1] = aot_tb->page_next[1]; */
    curr_tb->jmp_cp_data[0] = aot_tb->jmp_cp_data[0];
    curr_tb->jmp_cp_data[1] = aot_tb->jmp_cp_data[1];
    curr_tb->s_data->tu_generation_offset_patch[0] =
        aot_tb->tu_generation_offset_patch[0];
    curr_tb->s_data->tu_generation_offset_patch[1] =
        aot_tb->tu_generation_offset_patch[1];
    /* curr_tb->jmp_inst_hold[0] = aot_tb->jmp_inst_hold[0]; */
    /* curr_tb->jmp_inst_hold[1] = aot_tb->jmp_inst_hold[1]; */


    int64_t pc_diff = (int64_t)curr_tb->pc - (int64_t)aot_tb->pc;
    if (aot_tb->next_pc[0]) {
        curr_tb->next_pc[0] = aot_tb->next_pc[0] + pc_diff;
    } else {
        curr_tb->next_pc[0] = 0;
    }
    if (aot_tb->next_pc[1]) {
        curr_tb->next_pc[1] = aot_tb->next_pc[1] + pc_diff;
    } else {
        curr_tb->next_pc[1] = 0;
    }
    curr_tb->s_data->x86_next_pc = aot_tb->x86_next_pc + pc_diff;
#ifdef CONFIG_LATX_TU
    curr_tb->tc.offset_in_tu = aot_tb->tc.offset_in_tu;
    curr_tb->s_data->x86_target_pc = aot_tb->x86_target_pc + pc_diff;
    curr_tb->s_data->tu_size = aot_tb->tu_size;
    /* curr_tb->s_data->is_first_tb = 0; */
    /* curr_tb->s_data->last_ir1_type = 0; */
    /* curr_tb->s_data->tu_tb_mode = TU_TB_MODE_NONE; */
    curr_tb->s_data->tu_jmp[TU_TB_INDEX_TARGET] =
        aot_tb->tu_jmp[TU_TB_INDEX_TARGET];
    curr_tb->s_data->tu_jmp[TU_TB_INDEX_NEXT] =
        aot_tb->tu_jmp[TU_TB_INDEX_NEXT];
    curr_tb->s_data->eflag_use = aot_tb->eflag_use;
    curr_tb->s_data->eflag_use_real = aot_tb->eflag_use_real;
    curr_tb->s_data->last_ir1_type = aot_tb->last_ir1_type;
    curr_tb->s_data->tu_tb_mode = aot_tb->tu_tb_mode;
    /* curr_tb->tu_unlink_stub_offset = TU_UNLINK_STUB_INVALID; */
    curr_tb->ptn.eflag_type = aot_tb->ptn.eflag_type;
    if (curr_tb->ptn.eflag_type != PTN_EFLAG_TYPE_INVALID) {
        curr_tb->ptn.os = aot_tb->ptn.os;
        curr_tb->ptn.opnd0 = aot_tb->ptn.opnd0;
        curr_tb->ptn.opnd1 = aot_tb->ptn.opnd1;
        tb_set_ptn_pcoffset(curr_tb, aot_tb->ptn.pcoffset);
    }
#endif

#if DEBUG_AOT
    if (option_aot_debug) {
        fprintf(stderr, "tb pc %lx %lx\n", curr_tb->pc, aot_tb->pc);
        fprintf(stderr, "ir2_num %d %d\n", curr_tb->ir2_num, aot_tb->ir2_num);
        fprintf(stderr, "inst_ptr_offset %x %x\n",
                curr_tb->inst_ptr_offset, aot_tb->inst_ptr_offset);
        fprintf(stderr, "label_pos_nocheck %x %x\n",
                curr_tb->label_pos_nocheck, aot_tb->label_pos_nocheck);
        fprintf(stderr, "misc_flags %x %x\n", curr_tb->misc_flags, aot_tb->misc_flags);
    }

    recover_time += get_tsc() - begin;
#endif
    return curr_tb->ir2_num << 2;
}

#ifdef CONFIG_LATX_PRINT
int tb_aot_cnt[8];
#endif

static __attribute__((unused)) TranslationBlock *register_tb(vaddr exec_addr,
        CPUState *cpu, vaddr pc,
        uint64_t cs_base, aot_block *file_tb,
        aot_rel *rel_table, int rel_count, ENV *lsenv, uint64_t file_disp)
{
    CPUArchState *env = cpu->env_ptr;
    TranslationBlock *tb, *existing_tb;

    tb_page_addr_t phys_pc;
    tcg_insn_unit *gen_code_buf;
    void *host_pc;

    uint32_t mtcc_id = curr_mtcc(env);
    tcg_ctx = tcg_ctx_mtcc[mtcc_id];

    assert_memory_lock();
    qemu_thread_jit_write();

    phys_pc = get_page_addr_code_hostp(env, pc, &host_pc);

    tb = tcg_tb_alloc(tcg_ctx);
    if (unlikely(!tb)) {
        /* fprintf(stderr, "tcg_tb_alloc fail\n"); */
        return NULL;
    }

    gen_code_buf = tcg_ctx->code_gen_ptr;
    tb->pc = pc;
    tb->cs_base = cs_base;
#ifdef CONFIG_LATX_TU
    tu_reset_tb(tb);
#endif
    recover_tb_head(tb, file_tb);
    tb->tc.ptr = (void *)file_disp;
    tb_set_page_addr0(tb, phys_pc);
    tb_next_data_reset(tb);

    if (option_fast_itlb) {
        tb->hva = phys_pc & TARGET_PAGE_MASK;
    } else {
#ifdef CONFIG_SOFTMMU
        int error = 0;
        tb->hva = latxs_tr_cvt_cpu_addr_code(env, pc, &error);
#else
        tb->hva = 0;
#endif
    }
    tb_set_page_addr1(tb, -1);

#ifdef CONFIG_LATX_PROFILER
    CLN_TB_PROFILE(tb);
#endif
    /* init jump list */
    qemu_spin_init(&tb->jmp_lock);
    tb->jmp_list_head = (uintptr_t)NULL;
    tb->jmp_list_next[0] = (uintptr_t)NULL;
    tb->jmp_list_next[1] = (uintptr_t)NULL;
    tb->jmp_dest[0] = (uintptr_t)NULL;
    tb->jmp_dest[1] = (uintptr_t)NULL;

    /*
     * Insert TB into the corresponding region tree before publishing it
     * through QHT. Otherwise rewinding happened in the TB might fail to
     * lookup itself using host PC.
     */
    tcg_tb_insert(tb);

#ifdef CONFIG_LATX_PRINT
    if (tb->flags & HF_CPL_MASK) {
        tb_aot_cnt[cpu->cpu_index + 4]++;
    } else {
        tb_aot_cnt[cpu->cpu_index]++;
    }
#endif

    /*
     * No explicit memory barrier is required -- tb_link_page() makes the
     * TB visible in a consistent state.
     */
    tb->cflags |= CF_NO_ACTIVE | CF_INVALID;
    existing_tb = aot_tb_link_page(tb);

    /* fprintf(stderr, "register tb %lx tid %ld\n", tb->pc, syscall(SYS_gettid)); */

//    assert_no_pages_locked();
    /* if the TB already exists, discard what we just translated */
    if (unlikely(existing_tb != tb)) {
        fprintf(stderr, "existing_tb\n");
        uintptr_t orig_aligned = (uintptr_t)gen_code_buf;

        if (!option_split_tb) {
            orig_aligned -= ROUND_UP(sizeof(*tb), qemu_icache_linesize);
        }
        qatomic_set(&tcg_ctx->code_gen_ptr, (void *)orig_aligned);
        tcg_tb_remove(tb);
        return existing_tb;
    }
#if DEBUG_AOT
    register_tb_count++;
#endif

    return tb;
}

static G_NORETURN void aot_request_tb_flush(CPUState *cpu,
        uint32_t mtcc_id, tb_page_addr_t phys_pc)
{
    aot_unlock_load_pages(phys_pc);
    tb_flush_mtcc(cpu, mtcc_id);
#ifdef CONFIG_LATX_TU
    if (tu_runtime_enabled()) {
        tu_trees_reset();
        tu_reset_tb_flush();
    }
#endif
    cpu->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cpu);
}

static TranslationBlock *register_and_activate_tb(vaddr exec_addr,
        CPUState *cpu, vaddr pc,
        uint64_t cs_base, aot_block *file_tb,
        aot_rel *rel_table, int rel_count, ENV *lsenv, uint64_t file_disp)
{
    CPUArchState *env = cpu->env_ptr;
    TranslationBlock *tb, *existing_tb;
    tb_page_addr_t phys_pc;
    tcg_insn_unit *gen_code_buf;
    void *host_pc;
    int search_size;
    int gen_code_size = file_tb->ir2_num << 2;
    int copy_size = gen_code_size;
    size_t code_prefix = 0;

    uint32_t mtcc_id = curr_mtcc(env);
    tcg_ctx = tcg_ctx_mtcc[mtcc_id];

    assert_memory_lock();
    qemu_thread_jit_write();

    phys_pc = get_page_addr_code_hostp(env, pc, &host_pc);

    if (!option_split_search) {
        copy_size += file_tb->search_size;
    }
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    if (qemu_tcg_mode_2_lat()) {
        code_prefix = sizeof(struct TBMini);
    }
#endif
    code_prefix += CODE_GEN_ALIGN - 1;

    if ((uintptr_t)tcg_ctx->code_gen_ptr + code_prefix + copy_size >=
            (uintptr_t)tcg_ctx->code_gen_highwater &&
            tcg_region_alloc(tcg_ctx)) {
        aot_request_tb_flush(cpu, mtcc_id, phys_pc);
    }

    tb = tcg_tb_alloc(tcg_ctx);
    if (unlikely(!tb)) {
        aot_request_tb_flush(cpu, mtcc_id, phys_pc);
    }

    gen_code_buf = tcg_ctx->code_gen_ptr;
    tb->pc = pc;
    tb->cs_base = cs_base;
#ifdef CONFIG_LATX_TU
    tu_reset_tb(tb);
#endif
    recover_tb_head(tb, file_tb);
    tb->tc.ptr = (void *)file_disp;
    tb_set_page_addr0(tb, phys_pc);
    tb_next_data_reset(tb);

    if (option_fast_itlb) {
        tb->hva = phys_pc & TARGET_PAGE_MASK;
    } else {
#ifdef CONFIG_SOFTMMU
        int error = 0;
        tb->hva = latxs_tr_cvt_cpu_addr_code(env, pc, &error);
#else
        tb->hva = 0;
#endif
    }
    tb_set_page_addr1(tb, -1);

#ifdef CONFIG_LATX_PROFILER
    CLN_TB_PROFILE(tb);
#endif
    qemu_spin_init(&tb->jmp_lock);
    tb->jmp_list_head = (uintptr_t)NULL;
    tb->jmp_list_next[0] = (uintptr_t)NULL;
    tb->jmp_list_next[1] = (uintptr_t)NULL;
    tb->jmp_dest[0] = (uintptr_t)NULL;
    tb->jmp_dest[1] = (uintptr_t)NULL;

    tcg_tb_insert(tb);

#ifdef CONFIG_LATX_PRINT
    if (tb->flags & HF_CPL_MASK) {
        tb_aot_cnt[cpu->cpu_index + 4]++;
    } else {
        tb_aot_cnt[cpu->cpu_index]++;
    }
#endif

    tb->tc.ptr = (void *)tcg_splitwx_to_rx(gen_code_buf);

#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    if (qemu_tcg_mode_2_lat()) {
        uint64_t *tbm = (uint64_t *)gen_code_buf;
        tbmini_set_pointer(tbm, (uint64_t)tb);
        gen_code_buf = (tcg_insn_unit *)((uintptr_t)gen_code_buf + sizeof(struct TBMini));
        tb->tc.ptr = (void *)tcg_splitwx_to_rx(gen_code_buf);
    }
#endif

    uint32_t magic = *(uint32_t *)file_disp;
    if (magic != AOT_TB_MAGIC) {
        fprintf(stderr, "error tb magic %x in register_and_activate_tb\n", magic);
        _exit(0);
    }

    tb->misc_flags &= ~MF_FLAGS_NOEFLAGS;
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
    if (tb->flags == cpu_get_tb_cpu_state_flags_no_eflags(env)) {
        add_misc_flags(tb, MF_FLAGS_NOEFLAGS);
    }
#endif
    set_mask_to_miscflags(tb, curr_mtcc(env),
            MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT);

    uint32_t all_tb_size = *(uint32_t *)(file_disp + 8);
    aot_block *a_file_tb = (aot_block *)(file_disp + 16);
    aot_rel *curr_tb_rel_table =
        (aot_rel *)((void *)a_file_tb + all_tb_size);

    void *aot_ir2_code = (void *)((uintptr_t)a_file_tb + sizeof(aot_block));

    if (!option_split_search) {
        search_size = tb->search_size;
    } else {
        search_size = 0;
        if (!region_alloc_search(tb, mtcc_id)) {
            aot_request_tb_flush(cpu, mtcc_id, phys_pc);
        }
        memcpy((void *)tb->search_ptr, aot_ir2_code + (tb->ir2_num << 2),
                tb->icount << 2);
    }

    if (option_pushpop) {
        if (!region_alloc_push_pop_data(tb, mtcc_id, tb->icount)) {
            aot_request_tb_flush(cpu, mtcc_id, phys_pc);
        }
        memcpy(tb_opt_ptr(tb),
                aot_ir2_code + (tb->ir2_num << 2) + (tb->icount << 2),
                tb->icount << 2);
    }

    if (tb->tc.ptr + copy_size >= tcg_ctx->code_gen_highwater) {
        aot_request_tb_flush(cpu, mtcc_id, phys_pc);
    }

    memcpy((void *)tb->tc.ptr, aot_ir2_code, copy_size);
    aot_do_tb_reloc(lsenv, tb, curr_tb_rel_table, rel_count);

    tb->preorder_tb_offset = 0;
    tb->preorder_next = NULL;
#ifdef CONFIG_LATX_TU
    tb->ras_dest = (uintptr_t)NULL;
#endif
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
    if (tb->flags == cpu_get_tb_cpu_state_flags_no_eflags(env)) {
        add_misc_flags(tb, MF_FLAGS_NOEFLAGS);
    }
#endif
    if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
        if (qemu_tcg_mode_2_lat()) {
            latx_tb_reset_jmp_target(tb, 0);
        } else {
            tb_reset_jump(tb, 0);
        }
    }
    if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
        if (qemu_tcg_mode_2_lat()) {
            latx_tb_reset_jmp_target(tb, 1);
        } else {
            tb_reset_jump(tb, 1);
        }
    }

    {
        ADDR s = (ADDR)tb->tc.ptr;
        ADDR e = s + gen_code_size;
        flush_idcache_range((uintptr_t)s, (uintptr_t)s, (uintptr_t)e);
    }

#if defined(CONFIG_LATX_TCG_MODE) || defined(CONFIG_TCG_REGION_TREE_ENABLE)
    tb->tc.size = gen_code_size;
#endif

    uintptr_t sptr = (uintptr_t)gen_code_buf + gen_code_size;
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    if (qemu_tcg_mode_2_lat()) {
        sptr = sptr + sizeof(struct TBMini);
    }
#endif
    qatomic_set(&tcg_ctx->code_gen_ptr, (void *)
        ROUND_UP(sptr + search_size, CODE_GEN_ALIGN));

    tb->cflags &= ~(CF_NO_ACTIVE | CF_INVALID);
    existing_tb = aot_tb_link_page(tb);

    if (unlikely(existing_tb != tb)) {
        uintptr_t orig_aligned = (uintptr_t)gen_code_buf;
        if (!option_split_tb) {
            orig_aligned -= ROUND_UP(sizeof(*tb), qemu_icache_linesize);
        }
        qatomic_set(&tcg_ctx->code_gen_ptr, (void *)orig_aligned);
        tcg_tb_remove(tb);
        return existing_tb;
    }

#if DEBUG_AOT
    register_tb_count++;
    load_core_tb_count++;
#endif
    return tb;
}

static void aot_link_page_ras(CPUState *cpu, uint64_t cs_base,
        uint32_t flags, int cflags, vaddr page_pc, int page_tb_count,
        uint64_t file_disp)
{
    while (page_tb_count--) {
        uint32_t magic = *(uint32_t *)file_disp;
        if (magic != AOT_TB_MAGIC) {
            fprintf(stderr, "aot_link_page_ras error tb magic %x\n", magic);
            return;
        }

        uint32_t sum_size = *(uint32_t *)(file_disp + 4);
        aot_block *file_tb = (aot_block *)(file_disp + 16);
        vaddr tb_pc = page_pc + (file_tb->pc & ~TARGET_PAGE_MASK);
        uint32_t clean_cflags = file_tb->cflags &
                ~(CF_NO_ACTIVE | CF_INVALID);

        if (file_tb->cs_base == cs_base && file_tb->flags == flags &&
                clean_cflags == cflags) {
            TranslationBlock *tb = aot_tb_lookup(cpu, tb_pc,
                    cs_base, flags, clean_cflags);

            if (tb && tb->inst_ptr_offset &&
                    ((tb->pc & TARGET_PAGE_MASK) ==
                     (tb->s_data->x86_next_pc & TARGET_PAGE_MASK))) {
                TranslationBlock *next_tb = aot_tb_lookup(cpu,
                        tb->s_data->x86_next_pc, cs_base, flags, clean_cflags);
                if (next_tb && !latxs_ras_link(tb, next_tb)) {
                    latxs_ras_unlink(tb);
                }
            }
        }

        file_disp += sum_size;
    }
}

static TranslationBlock *register_page_tb(CPUState *cpu, vaddr pc,
        uint64_t cs_base, uint32_t flags, int cflags, vaddr hva,
        int page_tb_count, uint64_t file_disp, ENV *lsenv)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif
    bool perf_enabled = aot_perf_enabled();
    uint64_t perf_begin = perf_enabled ? aot_perf_now_ns() : 0;
    uint32_t original_page_tb_count = page_tb_count > 0 ? page_tb_count : 0;
    uint32_t registered_activated_count = 0;
    uint32_t aot_mode = (flags & HF_CPL_MASK) ? AOT_USER : AOT_CORE;

    TranslationBlock *ret_tb = NULL;
    uint32_t magic, all_tb_size, rel_count, offset, sum_size;
    vaddr page_pc = pc & TARGET_PAGE_MASK;
    uint64_t page_file_disp = file_disp;
    TranslationBlock *curr_tb;
    aot_rel *curr_tb_rel_table;

    /* bool not_can_do_hamt = !can_do_hamt(lsenv->cpu_state); */
    while (page_tb_count--) {
#if DEBUG_AOT
        uint64_t begin2 = get_tsc();
#endif
        magic = *(uint32_t *)file_disp;
        if (magic != AOT_TB_MAGIC) {
            fprintf(stderr, "register_page_tb error tb magic %x\n", magic);
            _exit(0);
        }
        sum_size = *(uint32_t *)(file_disp + 4);
        all_tb_size = *(uint32_t *)(file_disp + 8);
        rel_count = *(uint32_t *)(file_disp + 12);
        aot_block *curr_file_tb = (aot_block *)(file_disp + 16);

        /* if ( */
        /*         !(curr_file_tb->bool_flags & CAN_DO_HAMT) || */
        /*         (not_can_do_hamt != !(curr_file_tb->bool_flags & CAN_DO_HAMT)) */
        /*    ) { */
        /*     fprintf(stderr, "------------- not_can_do_hamt %d, curr_file_tb %d\n", not_can_do_hamt, */
        /*             (curr_file_tb->bool_flags & CAN_DO_HAMT)); */
        /*     file_disp += sum_size; */
        /*     continue; */
        /* } */

        curr_tb_rel_table =
            (aot_rel *)((void *)curr_file_tb + all_tb_size);
        offset = curr_file_tb->pc & ~TARGET_PAGE_MASK;

        if (curr_file_tb->cs_base != cs_base ||
                curr_file_tb->flags != flags ||
                (curr_file_tb->cflags & ~(CF_NO_ACTIVE | CF_INVALID)) !=
                    cflags) {
            file_disp += sum_size;
            continue;
        }

        if (!test_tb_reloc(curr_tb_rel_table, rel_count, page_pc + offset)) {
            file_disp += sum_size;
            continue;
        }

        TranslationBlock *existing_tb = aot_tb_lookup(cpu,
                page_pc + offset, cs_base, curr_file_tb->flags,
                curr_file_tb->cflags & ~(CF_INVALID | CF_NO_ACTIVE));
#if DEBUG_AOT
        uint64_t begin3 = get_tsc();
        t1 += begin3 - begin2;
#endif
        if (existing_tb) {
            if (existing_tb->pc == pc) {
                ret_tb = existing_tb;
            }
            file_disp += sum_size;
            continue;
        }
        curr_tb = register_and_activate_tb(pc, cpu, page_pc + offset, cs_base,
                    curr_file_tb, curr_tb_rel_table, rel_count, lsenv, file_disp);
#if DEBUG_AOT
        t2 += get_tsc() - begin3;
#endif
        if (!curr_tb) {
            fprintf(stderr, "[AOT] Load: stop registering page after TB recover failure\n");
            break;
        }
        registered_activated_count++;
        if (curr_tb->pc == pc
                && curr_tb->flags == flags
                && (curr_tb->cflags & ~(CF_NO_ACTIVE | CF_INVALID)) == cflags) {
            ret_tb = curr_tb;
        }
        file_disp += sum_size;
    }

    if (option_soft_ras) {
        aot_link_page_ras(cpu, cs_base, flags, cflags, page_pc,
                original_page_tb_count, page_file_disp);
    }

#if DEBUG_AOT
    register_page_tb_time += get_tsc() - begin;
#endif
    if (perf_enabled) {
        aot_perf_record_register_activate_tb(aot_mode, original_page_tb_count,
                registered_activated_count, aot_perf_now_ns() - perf_begin);
    }
    return ret_tb;
}

TranslationBlock *register_page(CPUState *cpu, vaddr pc, uint64_t cs_base, uint32_t flags,
        int cflags, ENV *lsenv, uint64_t phys_pc)
{

#ifdef CONFIG_SOFTMMU
    CPUArchState *env = cpu->env_ptr;
    int error = 0;
    ADDR hva = latxs_tr_cvt_cpu_addr_code(env, pc & TARGET_PAGE_MASK, &error);
    assert(!error);
#else
    ADDR hva = 1;
#endif
    TranslationBlock *tb = NULL;
#ifdef CONFIG_SOFTMMU
    uint64_t aot_mesage __attribute__((unused)) = get_page_aot_mesage(phys_pc >> TARGET_PAGE_BITS);
#else
    uint64_t aot_mesage __attribute__((unused)) = 0;
#endif
    uint64_t file_addr = find_page(hva);
    if (file_addr) {
        page_head *ph = (page_head *)file_addr;
        tb = register_page_tb(cpu, pc, cs_base, flags, cflags,
                hva, ph->tb_count, file_addr + sizeof(page_head) + TARGET_PAGE_SIZE, lsenv);
#ifdef CONFIG_SOFTMMU
        set_page_aot_mesage(phys_pc >> TARGET_PAGE_BITS, PAGE_LOADED, pc);
    } else {
        set_page_aot_mesage(phys_pc >> TARGET_PAGE_BITS, PAGE_NO_FIND, pc);
#endif
    }

    return tb;
}

TranslationBlock *load_aot_tb(CPUState *cpu, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags, ENV *lsenv)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif
    CPUArchState *env = cpu->env_ptr;

    void *host_pc;
    uint64_t phys_pc = get_page_addr_code_hostp(env, pc, &host_pc);
    if (phys_pc == -1) {
        return NULL;
    }

#ifdef CONFIG_SOFTMMU
    if (!aot_host_pc_in_main_ram(host_pc)) {
        return NULL;
    }
#endif

    tb_lock_page0(phys_pc);

#ifdef CONFIG_SOFTMMU
#ifdef CONFIG_LATX_HACK_MEM
    if (!hamt_ptwdata_smc_page_is_protected(phys_pc)) {
        tlb_protect_code(phys_pc);
    }
#else
    tlb_protect_code(phys_pc);
#endif
#endif

    TranslationBlock *tb = NULL;

#ifdef CONFIG_SOFTMMU
    uint64_t aot_mesage = get_page_aot_mesage(phys_pc >> TARGET_PAGE_BITS);
#else
    uint64_t aot_mesage = 0;
#endif

    /* fprintf(stderr, "load_aot_tb page %lx pc %lx cflags %lx flags %lx cs_base %lx\n", */
    /*         phys_pc & TARGET_PAGE_MASK, pc, cflags, flags, cs_base); */

    if ((aot_mesage & 0xf) == PAGE_NO_LOAD) {
        tb = register_page(cpu, pc, cs_base, flags, cflags, lsenv, phys_pc);
    }
    else if ((aot_mesage & 0xf) == PAGE_LOADED) {
        tb = aot_tb_lookup(cpu, pc, cs_base, flags, cflags | CF_INVALID | CF_NO_ACTIVE);
        if (!tb) {
            tb = register_page(cpu, pc, cs_base, flags, cflags, lsenv, phys_pc);
        }
    } else {
        /* unlock_page(phys_pc); */
        /* load_aot_time += (get_tsc() - begin); */
        /* return NULL; */
    }

    if (tb && (tb->cflags & CF_NO_ACTIVE)) {
        tb = activate_tb(tb, cpu, lsenv);
    } else {
#if DEBUG_AOT
        mis_tb++;
#endif
    }

#ifdef CONFIG_SOFTMMU
    unlock_page(phys_pc);
#endif
#if DEBUG_AOT
    load_aot_time += (get_tsc() - begin);
#endif

    return tb;
}

#ifdef CONFIG_LATX_TU
static inline aot_tu_page *aot_tu_pages(aot_tu_head *tu_head)
{
    return (aot_tu_page *)(tu_head + 1);
}

static inline aot_block *aot_tu_blocks(aot_tu_head *tu_head)
{
    return (aot_block *)(aot_tu_pages(tu_head) + tu_head->page_count);
}

static inline vaddr aot_tu_rebase_pc(const aot_tu_head *tu_head,
                                     vaddr anchor_page, vaddr saved_pc)
{
    vaddr saved_page = tu_head->first_pc & TARGET_PAGE_MASK;

    return anchor_page + (saved_pc - saved_page);
}

static int aot_phys_page_cmp(const void *ap, const void *bp)
{
    tb_page_addr_t a = *(const tb_page_addr_t *)ap;
    tb_page_addr_t b = *(const tb_page_addr_t *)bp;

    return a < b ? -1 : a > b;
}

/* Record bounds and cross-page reference uniqueness were checked at open. */
static bool aot_page_contains_tu_dependency(const page_head *page,
        const aot_tu_head *tu, int64_t pc_delta)
{
    const uint8_t *record = (const uint8_t *)(page + 1) + TARGET_PAGE_SIZE;

    for (uint32_t i = 0; i < page->tb_count; i++) {
        uint32_t magic = *(const uint32_t *)record;

        if (magic == AOT_TB_MAGIC) {
            const aot_tu_head *payload = (const aot_tu_head *)record;

            /* Single-page TU IDs may repeat: match the exact payload. */
            if (!pc_delta && payload == tu) {
                return true;
            }
            record += payload->tu_sum_size;
        } else if (magic == AOT_TU_REF_MAGIC) {
            const aot_tu_ref *ref = (const aot_tu_ref *)record;

            if (pc_delta && ref->tu_id == tu->tu_id &&
                    ref->anchor_delta == -pc_delta) {
                return true;
            }
            record += sizeof(*ref);
        } else {
            return false;
        }
    }
    return false;
}

static bool aot_tu_dependency_matches(const void *host_page,
        const aot_tu_head *tu, int64_t pc_delta)
{
    uint32_t h = get_page_hash((uint32_t *)host_page);
    uint32_t idx = h & (aot_hash_size - 1);

    /*
     * All dependency pages are locked and write-protected. The sampled hash
     * only selects candidates; compare every byte against this TU's saved
     * page. Continue past equal-content records belonging to other TUs.
     */
    for (uint32_t i = 0; i < aot_hash_size; i++) {
        uint64_t offset = hash_table[idx];

        if (offset == UINT64_MAX) {
            break;
        }
        const page_head *page = (const page_head *)((const uint8_t *)
                in_tb_ptr + offset);

        if (page->page_hash == h &&
                aot_page_contains_tu_dependency(page, tu, pc_delta) &&
                !memcmp(host_page, page + 1, TARGET_PAGE_SIZE)) {
            return true;
        }
        idx = (idx + 1) & (aot_hash_size - 1);
    }
    return false;
}

static bool aot_lock_tu_dependencies(CPUState *cpu, vaddr anchor_page,
        aot_tu_head *tu_head)
{
    AOTTUPageDependency *deps;
    tb_page_addr_t *phys_pages = NULL;
    CPUArchState *env = cpu->env_ptr;
    aot_tu_page *pages = aot_tu_pages(tu_head);
    int deps_num = tu_head->page_count;
    int phys_num = 0;
    bool ok = false;

    g_assert(aot_tu_locked_pages_num == 0);
    g_assert(aot_tu_dependencies_num == 0);
    if (deps_num <= 0 || deps_num > AOT_TU_MAX_PAGES) {
        return false;
    }
    deps = g_try_new(AOTTUPageDependency, deps_num);
    phys_pages = g_try_new(tb_page_addr_t, deps_num);
    if (!deps || !phys_pages) {
        goto out;
    }
    for (int i = 0; i < deps_num; i++) {
        deps[i] = (AOTTUPageDependency) {
            .vp = anchor_page + pages[i].pc_delta,
            .pp = -1,
            .pc_delta = pages[i].pc_delta,
        };
    }

    for (int retry = 0; retry < 8; retry++) {
        bool stable = true;

        phys_num = 0;
        for (int i = 0; i < deps_num; i++) {
            void *host_page = NULL;
            tb_page_addr_t pp = aot_probe_code_page(env, deps[i].vp,
                                                   &host_page);
            if (pp == (tb_page_addr_t)-1 ||
                    !aot_host_pc_in_main_ram(host_page)) {
                goto out;
            }
            deps[i].pp = pp & TARGET_PAGE_MASK;
            phys_pages[phys_num++] = deps[i].pp;
#ifdef CONFIG_SOFTMMU
#ifdef CONFIG_LATX_HACK_MEM
            if (!tlb_code_page_is_protected(deps[i].pp)) {
                tlb_protect_code(deps[i].pp);
            }
#else
            /*
             * The protection query is a stub without HACK_MEM. Protect every
             * dependency before comparing its bytes and registering TBs.
             */
            tlb_protect_code(deps[i].pp);
#endif
#endif
        }

        qsort(phys_pages, phys_num, sizeof(*phys_pages), aot_phys_page_cmp);
        int unique_num = 0;
        for (int i = 0; i < phys_num; i++) {
            if (!unique_num || phys_pages[i] != phys_pages[unique_num - 1]) {
                phys_pages[unique_num++] = phys_pages[i];
            }
        }
        phys_num = unique_num;
        for (int i = 0; i < phys_num; i++) {
            tb_lock_page0(phys_pages[i]);
        }

        for (int i = 0; i < deps_num; i++) {
            void *host_page = NULL;
            tb_page_addr_t pp = aot_probe_code_page(
                    env, deps[i].vp, &host_page);

            if (pp == (tb_page_addr_t)-1 ||
                    (pp & TARGET_PAGE_MASK) != deps[i].pp
#ifdef CONFIG_SOFTMMU
                    || !tlb_code_page_is_protected(deps[i].pp)
#endif
                    ) {
                stable = false;
                break;
            }
            if (!aot_tu_dependency_matches(host_page, tu_head,
                                            deps[i].pc_delta)) {
                /*
                 * Stable mapping but wrong content: this candidate cannot
                 * be used. Do not repeat identical checks eight times.
                 */
                for (int j = phys_num - 1; j >= 0; j--) {
                    tb_unlock_page0(phys_pages[j]);
                }
                goto out;
            }
            deps[i].host_page = host_page;
        }
        if (stable) {
            memcpy(aot_tu_dependencies, deps, deps_num * sizeof(*deps));
            aot_tu_dependencies_num = deps_num;
            aot_tu_locked_pages = phys_pages;
            aot_tu_locked_pages_num = phys_num;
            phys_pages = NULL;
            ok = true;
            goto out;
        }
        for (int i = phys_num - 1; i >= 0; i--) {
            tb_unlock_page0(phys_pages[i]);
        }
    }

out:
    g_free(phys_pages);
    g_free(deps);
    return ok;
}

/* Consume only the mapping snapshot validated while all page locks are held. */
static tb_page_addr_t aot_locked_tu_page(vaddr pc, void **hostp)
{
    vaddr vp = pc & TARGET_PAGE_MASK;
    vaddr offset = pc & ~TARGET_PAGE_MASK;

    g_assert(aot_tu_locked_pages_num > 0);
    for (int i = 0; i < aot_tu_dependencies_num; i++) {
        const AOTTUPageDependency *dep = &aot_tu_dependencies[i];

        if (dep->vp == vp) {
            if (hostp) {
                *hostp = (char *)dep->host_page + offset;
            }
            return dep->pp + offset;
        }
    }
    return -1;
}

static TranslationUnitPages *aot_build_tu_pages_desc(
        const aot_tu_head *tu_head, vaddr anchor_page, TCGContext *ctx)
{
    const aot_tu_page *pages = aot_tu_pages((aot_tu_head *)tu_head);
    TranslationUnitPages *desc = region_alloc_tu_pages(ctx);

    if (!desc) {
        return NULL;
    }
    memset(desc, 0xff, sizeof(*desc));
    desc->id = tu_pages_alloc_id();
    for (uint32_t i = 0; i < tu_head->page_count; i++) {
        vaddr vp = anchor_page + pages[i].pc_delta;
        tb_page_addr_t pp = aot_locked_tu_page(vp, NULL);

        if (pp == (tb_page_addr_t)-1) {
            return NULL;
        }
        pp &= TARGET_PAGE_MASK;
        desc->pages[i].vp_phys_hi = tu_page_desc_pack_vp(vp, pp);
        desc->pages[i].mix = tb_cal_page_addr_mix(vp, pp);
    }
    return desc;
}

TranslationBlock *load_tu(CPUState *cpu, vaddr pc,
        uint64_t cs_base, uint32_t flags, int cflags,
        vaddr hva, uint64_t file_disp, vaddr anchor_page, ENV *lsenv,
        uint32_t *loaded_tb_count)
{
    aot_tu_head *tu_head = (aot_tu_head *)file_disp;
    if (tu_head->tb_magic != AOT_TB_MAGIC) {
        fprintf(stderr, "error tu magic %x in load_tu\n", tu_head->tb_magic);
        _exit(0);
    }
    int tb_num_in_tu = tu_head->tb_count;
    uint32_t loaded_count = 0;

    if (loaded_tb_count) {
        *loaded_tb_count = 0;
    }

    TranslationBlock *tb;
    aot_block *aot_tb;
    TranslationBlock *ret = NULL;
    aot_block *file_tb_vector = aot_tu_blocks(tu_head);
    int ir2_size = file_tb_vector[0].tu_size;
    aot_rel *curr_rel_table =
        (aot_rel *)((uintptr_t)(file_tb_vector + tu_head->tb_count) + ir2_size);
    uintptr_t search_code_pos = (uintptr_t)curr_rel_table + tu_head->rel_count * sizeof(aot_rel);

    void *host_pc;
    tb_page_addr_t phys_pc;
    tb_page_addr_t locked_phys_pc;
    CPUArchState *env = cpu->env_ptr;
    uint32_t mtcc_id = curr_mtcc(env);
    tcg_ctx = tcg_ctx_mtcc[mtcc_id];
    assert_memory_lock();
    qemu_thread_jit_write();
    locked_phys_pc = aot_locked_tu_page(pc, NULL);
    if (locked_phys_pc == (tb_page_addr_t)-1) {
        return NULL;
    }

    size_t code_prefix = CODE_GEN_ALIGN - 1;
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    code_prefix += sizeof(struct TBMini) * (tb_num_in_tu + 1);
#endif
    if ((uintptr_t)tcg_ctx->code_gen_ptr + code_prefix + ir2_size >=
            (uintptr_t)tcg_ctx->code_gen_highwater &&
            tcg_region_alloc(tcg_ctx)) {
        aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
    }
    bool existing_tu = false;
    for (int i = 0; i < tb_num_in_tu; i++) {
        aot_tb = &file_tb_vector[i];
        vaddr curr_pc = aot_tu_rebase_pc(tu_head, anchor_page,
                                         aot_tb->pc);
        if (aot_locked_tu_page(curr_pc, NULL) == (tb_page_addr_t)-1 ||
                (aot_tb->page_addr[1] != (tb_page_addr_t)-1 &&
                 aot_locked_tu_page((curr_pc & TARGET_PAGE_MASK) +
                     TARGET_PAGE_SIZE, NULL) == (tb_page_addr_t)-1)) {
            return NULL;
        }
        uint32_t rel_count = aot_tb->rel_start < 0 ? 0 :
            aot_tb->rel_end - aot_tb->rel_start + 1;
        if (rel_count &&
                !test_tb_reloc(curr_rel_table + aot_tb->rel_start,
                    rel_count, curr_pc)) {
            return NULL;
        }
        /* if (not_can_do_hamt != !(aot_tb->bool_flags & CAN_DO_HAMT)) { */
        /*     fprintf(stderr, "can_do_hamt not equal curr %x aot %x\n", */
        /*             !not_can_do_hamt, aot_tb->bool_flags & CAN_DO_HAMT); */
        /*     return NULL; */
        /* } */
        int saved_error_code = env->error_code;
        TranslationBlock *existing = tb_htable_lookup_noexit(cpu, curr_pc,
                cs_base, aot_tb->flags, aot_tb->cflags);
        env->error_code = saved_error_code;
        if (existing) {
            existing_tu = true;
            if (curr_pc == pc && aot_tb->flags == flags &&
                    aot_tb->cflags == cflags) {
                ret = existing;
            }
        }
    }
    if (existing_tu) {
        return ret;
    }

    TranslationUnitPages *tu_pages = NULL;
    if (tu_head->page_count > 1 || tucp_fast_needs_pages_desc()) {
        tu_pages = aot_build_tu_pages_desc(tu_head, anchor_page,
                                          tcg_ctx);
        if (!tu_pages) {
            aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
        }
    }

    int pre_search_size = 0;
    g_autofree TranslationBlock **curr_tb_vector =
        g_try_new0(TranslationBlock *, tu_head->tb_count);
    if (!curr_tb_vector) {
        return (TranslationBlock *)-1;
    }
    for (int i = 0; i < tb_num_in_tu; i++) {
        curr_tb_vector[i] = tcg_tb_alloc_full(tcg_ctx);
        tb = curr_tb_vector[i];
        if (unlikely(!tb)) {
            aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
        }
        tu_reset_tb(tb);
        vaddr curr_pc = aot_tu_rebase_pc(tu_head, anchor_page,
                                         file_tb_vector[i].pc);
        phys_pc = aot_locked_tu_page(curr_pc, &host_pc);
        g_assert(phys_pc != (tb_page_addr_t)-1);
        tb->pc = curr_pc;
        tb->cs_base = cs_base;

        recover_tb_head(tb, &file_tb_vector[i]);
        TB_TU_PAGES(tb) = tu_pages;

        tb->misc_flags &= ~MF_FLAGS_NOEFLAGS;
#if defined(CONFIG_TB_FLAGS_NO_EFLAGS)
        if (tb->flags == cpu_get_tb_cpu_state_flags_no_eflags(env)) {
            add_misc_flags(tb, MF_FLAGS_NOEFLAGS);
        }
#endif
        /* set_mask_to_miscflags(tb, curr_mtcc(env), */
        /*         MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT); */

        tb_set_page_addr0(tb, phys_pc);
        tb_next_data_reset(tb);
        if (option_fast_itlb) {
            tb->hva = phys_pc & TARGET_PAGE_MASK;
        } else {
            tb->hva = (uintptr_t)host_pc;
        }
        if (file_tb_vector[i].page_addr[1] != -1) {
            vaddr page1_pc = (curr_pc & TARGET_PAGE_MASK) + TARGET_PAGE_SIZE;
            tb_page_addr_t phys_page1 =
                aot_locked_tu_page(page1_pc, NULL);
            g_assert(phys_page1 != (tb_page_addr_t)-1);
            tb_set_page_addr1(tb, phys_page1);
        } else {
            tb_set_page_addr1(tb, -1);
        }
        /* latx_mmio_tb_check(tb); */
#ifdef CONFIG_LATX_PROFILER
        CLN_TB_PROFILE(tb);
#endif
        /* init jump list */
        qemu_spin_init(&tb->jmp_lock);
        tb->jmp_list_head = (uintptr_t)NULL;
        tb->jmp_list_next[0] = (uintptr_t)NULL;
        tb->jmp_list_next[1] = (uintptr_t)NULL;
        tb->jmp_dest[0] = (uintptr_t)NULL;
        tb->jmp_dest[1] = (uintptr_t)NULL;
        tb->s_data->is_invalidate = false;
    }

    uintptr_t tbmini_ptr = 0;
#if defined(CONFIG_TCG_REGION_TREE_ENABLE)
    qatomic_set(&tcg_ctx->code_gen_ptr, (void *)
            ROUND_UP((uintptr_t)tcg_ctx->code_gen_ptr, CODE_GEN_ALIGN));
#else

    qatomic_set(&tcg_ctx->code_gen_ptr, ROUND_UP((uintptr_t)tcg_ctx->code_gen_ptr, CODE_GEN_ALIGN));

    tbmini_ptr = (uintptr_t)
                ROUND_UP((uintptr_t)tcg_ctx->code_gen_ptr, CODE_GEN_ALIGN);
    uintptr_t last_tbmini_ptr = tbmini_ptr +
                sizeof(struct TBMini) * (tb_num_in_tu + 1);
    qatomic_set(&tcg_ctx->code_gen_ptr, last_tbmini_ptr);
    /* First tbm save tb_num_in_tu and TB_MAGIC. */
    tu_tbmini_set_pointer((uint64_t *)tbmini_ptr, tb_num_in_tu, TB_MAGIC);
    uintptr_t curr_tbmini_ptr = (uintptr_t)tcg_ctx->code_gen_ptr -
                        sizeof(struct TBMini) * (tb_num_in_tu + 1);
#endif

    tcg_insn_unit *gen_code_buf = (tcg_insn_unit *)tcg_splitwx_to_rx(tcg_ctx->code_gen_ptr);
    tb = curr_tb_vector[0];
    tb->tc.ptr = gen_code_buf;
    if ((uintptr_t)gen_code_buf + ir2_size >= (uintptr_t)tcg_ctx->code_gen_highwater) {
        aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
    }
    void *aot_ir2_code = file_tb_vector + tu_head->tb_count;
    memcpy(gen_code_buf, aot_ir2_code, ir2_size);

    qatomic_set(&tcg_ctx->code_gen_ptr, (void *)
            ROUND_UP((uintptr_t)(gen_code_buf) + ir2_size, CODE_GEN_ALIGN));

    for (int i = 0; i < tu_head->tb_count; i++) {
        tb = curr_tb_vector[i];

#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
        uint64_t size = tb->ir2_num << 2;
        curr_tbmini_ptr += sizeof(struct TBMini);
        tu_tbmini_set_pointer((uint64_t *)curr_tbmini_ptr, (uint64_t)tb, size);
#endif

        if (i) {
            tb->tc.ptr = (void *)((uintptr_t)gen_code_buf + tb->tc.offset_in_tu);
        }
        uint32_t rel_count = file_tb_vector[i].rel_start < 0 ? 0 :
            file_tb_vector[i].rel_end - file_tb_vector[i].rel_start + 1;
        if (rel_count) {
            aot_do_tb_reloc(lsenv, tb,
                    curr_rel_table + file_tb_vector[i].rel_start, rel_count);
        }
        if (!option_split_search) {
            fprintf(stderr, "no support !option_split_search\n");
            exit(0);
        } else {
            if (!region_alloc_search(tb, mtcc_id)) {
                aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
            }
            memcpy((void*)tb->search_ptr,
                    (void *)(search_code_pos + pre_search_size), tb->icount << 2);
            pre_search_size += tb->icount << 2;
        }
        tb->s_data->next_tb[0] = NULL;
        tb->s_data->next_tb[1] = NULL;
        if (i) {
            tb->s_data->next_tb[0] = curr_tb_vector[i - 1];
        }
        if (i != tu_head->tb_count - 1) {
            tb->s_data->next_tb[1] = curr_tb_vector[i + 1];
        }
        tb->bool_flags |= IS_TU_TB;
    }

    flush_idcache_range((uintptr_t)gen_code_buf, (uintptr_t)gen_code_buf,
                        ir2_size);

    if (option_pushpop) {
        for (int i = 0; i < tu_head->tb_count; i++) {
            tb = curr_tb_vector[i];
            if (!region_alloc_push_pop_data(tb, mtcc_id, tb->icount)) {
                aot_request_tb_flush(cpu, mtcc_id, locked_phys_pc);
            }
            memcpy(tb_opt_ptr(tb), (void *)(search_code_pos + pre_search_size),
                    tb->icount << 2);
            pre_search_size += tb->icount << 2;
        }
    }

    for (int i = 0; i < tu_head->tb_count - 1; i++) {
        tb = curr_tb_vector[i];
        if (option_soft_ras && tb->inst_ptr_offset) {
            TranslationBlock *next_tb = curr_tb_vector[i + 1];
            if (tb->s_data->x86_next_pc == next_tb->pc) {
                if (!latxs_ras_link(tb, next_tb)) {
                    latxs_ras_unlink(tb);
                }
            } else {
                /* latxs_ras_unlink(tb); */
            }
        }
    }

    for (int i = 0; i < tu_head->tb_count; i++) {
        tb = curr_tb_vector[i];
        tcg_tb_insert(tb);
#ifdef CONFIG_LATX_PRINT
        if (tb->flags & HF_CPL_MASK) {
            tb_aot_cnt[cpu->cpu_index + 4]++;
        } else {
            tb_aot_cnt[cpu->cpu_index]++;
        }
#endif
        aot_tb_link_page(tb);
        loaded_count++;
#if DEBUG_AOT
        load_user_tb_count++;
#endif
        if (tb->pc == pc &&
                tb->cflags == cflags &&
                tb->flags == flags) {
            ret = tb;
        }
    }

    check_tu(cpu, tu_head, curr_tb_vector, file_tb_vector, tbmini_ptr, ir2_size);
    if (loaded_tb_count) {
        *loaded_tb_count = loaded_count;
    }
    return ret;
}

static bool aot_tu_matches(const aot_tu_head *tu_head,
        vaddr anchor_page, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags)
{
    aot_block *blocks = aot_tu_blocks((aot_tu_head *)tu_head);

    for (uint32_t i = 0; i < tu_head->tb_count; i++) {
        if (aot_tu_rebase_pc(tu_head, anchor_page, blocks[i].pc) == pc &&
                blocks[i].cs_base == cs_base &&
                blocks[i].flags == flags &&
                blocks[i].cflags == cflags) {
            return true;
        }
    }
    return false;
}

static bool aot_tu_has_page(const aot_tu_head *tu_head, int64_t pc_delta)
{
    const aot_tu_page *pages = aot_tu_pages((aot_tu_head *)tu_head);

    for (uint32_t i = 0; i < tu_head->page_count; i++) {
        if (pages[i].pc_delta == pc_delta) {
            return true;
        }
    }
    return false;
}

static TranslationBlock *aot_try_load_tu(CPUState *cpu, vaddr pc,
        uint64_t cs_base, uint32_t flags, int cflags, vaddr hva,
        uint64_t file_disp, vaddr anchor_page, ENV *lsenv,
        uint32_t *candidate_tb_count, uint32_t *loaded_tb_count)
{
    aot_tu_head *tu_head = (aot_tu_head *)file_disp;

    if ((tu_head->page_count > 1 && !aot_cross_page_tu_enabled()) ||
            !aot_tu_matches(tu_head, anchor_page, pc, cs_base, flags,
                            cflags)) {
        return NULL;
    }
    *candidate_tb_count += tu_head->tb_count;
    if (!aot_lock_tu_dependencies(cpu, anchor_page, tu_head)) {
        return NULL;
    }

    TranslationBlock *tb = load_tu(cpu, pc, cs_base, flags, cflags, hva,
            file_disp, anchor_page, lsenv, loaded_tb_count);
    aot_unlock_load_pages(-1);
    return tb;
}

static TranslationBlock *aot_load_tu_ref(CPUState *cpu, vaddr pc,
        uint64_t cs_base, uint32_t flags, int cflags,
        const aot_tu_ref *ref, vaddr container_page, ENV *lsenv,
        uint32_t *candidate_tb_count, uint32_t *loaded_tb_count)
{
    vaddr anchor_page = container_page + ref->anchor_delta;
    ADDR anchor_hva = 0;
    uint64_t anchor_file = aot_find_tu_page_record(cpu, anchor_page,
            &anchor_hva, NULL);

    if (!anchor_file) {
        return NULL;
    }

    page_head *anchor_head = (page_head *)anchor_file;
    uint64_t record = anchor_file + sizeof(*anchor_head) + TARGET_PAGE_SIZE;
    for (uint32_t i = 0; i < anchor_head->tb_count; i++) {
        uint32_t magic = *(uint32_t *)record;

        if (magic == AOT_TB_MAGIC) {
            aot_tu_head *tu_head = (aot_tu_head *)record;
            if (tu_head->tu_id == ref->tu_id &&
                    aot_tu_has_page(tu_head, -ref->anchor_delta)) {
                TranslationBlock *tb = aot_try_load_tu(cpu, pc, cs_base,
                        flags, cflags, anchor_hva, record, anchor_page, lsenv,
                        candidate_tb_count, loaded_tb_count);
                if (tb) {
                    return tb;
                }
            }
            record += tu_head->tu_sum_size;
        } else if (magic == AOT_TU_REF_MAGIC) {
            record += sizeof(aot_tu_ref);
        } else {
            return NULL;
        }
    }
    return NULL;
}

TranslationBlock *load_page_tu(CPUState *cpu, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags, vaddr hva, uint64_t file_disp, ENV *lsenv)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif
    bool perf_enabled = aot_perf_enabled();
    uint64_t perf_begin = perf_enabled ? aot_perf_now_ns() : 0;
    TranslationBlock *ret_tb = NULL;
    page_head *ph = (page_head *)file_disp;
    TranslationBlock *curr_tb;
    uint32_t record_count = 0;
    uint32_t candidate_tb_count = 0;
    uint32_t registered_activated_count = 0;
    vaddr container_page = pc & TARGET_PAGE_MASK;
    file_disp += sizeof(page_head) + TARGET_PAGE_SIZE;

    while (record_count < ph->tb_count) {
#if DEBUG_AOT
        uint64_t gen_code_begin = get_tsc();
#endif
        uint32_t loaded_tb_count = 0;
        uint32_t magic = *(uint32_t *)file_disp;
        curr_tb = NULL;
        if (magic == AOT_TB_MAGIC) {
            aot_tu_head *tu_head = (aot_tu_head *)file_disp;

            curr_tb = aot_try_load_tu(cpu, pc, cs_base, flags, cflags,
                    hva, file_disp, container_page, lsenv,
                    &candidate_tb_count, &loaded_tb_count);
            file_disp += tu_head->tu_sum_size;
        } else if (magic == AOT_TU_REF_MAGIC) {
            aot_tu_ref *ref = (aot_tu_ref *)file_disp;

            if (aot_cross_page_tu_enabled()) {
                curr_tb = aot_load_tu_ref(cpu, pc, cs_base, flags, cflags,
                        ref, container_page, lsenv, &candidate_tb_count,
                        &loaded_tb_count);
            }
            file_disp += sizeof(*ref);
        } else {
            break;
        }
        registered_activated_count += loaded_tb_count;
#if DEBUG_AOT
        load_tu_time += get_tsc() - gen_code_begin;
#endif
        if ((uintptr_t)curr_tb == -1) {
            break;
        }
        if (curr_tb && curr_tb->pc == pc
                && curr_tb->flags == flags
                && curr_tb->cflags == cflags) {
            ret_tb = curr_tb;
            break;
        }
        record_count++;
    }

#if DEBUG_AOT
    load_page_tu_time += get_tsc() - begin;
#endif
    if (perf_enabled) {
        aot_perf_record_register_activate_tb(AOT_USER, candidate_tb_count,
                registered_activated_count, aot_perf_now_ns() - perf_begin);
    }
    return ret_tb;
}

TranslationBlock *load_aot_tu(CPUState *cpu, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags, ENV *lsenv)
{
#if DEBUG_AOT
    uint64_t begin = get_tsc();
#endif
    ADDR hva = 0;
    uint64_t file_addr = aot_find_tu_page_record(cpu,
            pc & TARGET_PAGE_MASK, &hva, NULL);

    if (!file_addr) {
        return NULL;
    }
    TranslationBlock *tb = load_page_tu(cpu, pc, cs_base, flags, cflags,
                                        hva, file_addr, lsenv);
#if DEBUG_AOT
    load_aot_time += (get_tsc() - begin);
#endif

    return tb;
}
#endif

static bool aot_load_select_index(uint32_t flags,
        AOTLoadIndexSelection *selection)
{
    AOTPublishedFile *file;

    if (flags & HF_CPL_MASK) {
        file = qatomic_rcu_read(&user_aot_file);
        *selection = (AOTLoadIndexSelection) {
            .file = file,
            .tb_ptr = file ? file->tb_ptr : NULL,
            .hash_table = file ? file->hash_table : NULL,
            .hash_size = file ? file->hash_size : 0,
            .file_generation = file ? file->generation : 0,
            .aot_mode = AOT_USER,
#ifdef CONFIG_LATX_TU
            .use_tu = aot_tu_enabled(),
#else
            .use_tu = false,
#endif
        };
    } else {
        file = qatomic_rcu_read(&core_aot_file);
        *selection = (AOTLoadIndexSelection) {
            .file = file,
            .tb_ptr = file ? file->tb_ptr : NULL,
            .hash_table = file ? file->hash_table : NULL,
            .hash_size = file ? file->hash_size : 0,
            .file_generation = file ? file->generation : 0,
            .aot_mode = AOT_CORE,
            .use_tu = false,
        };
    }

    return selection->tb_ptr != NULL;
}

static void aot_load_bind_index(const AOTLoadIndexSelection *selection)
{
    in_tb_ptr = selection->tb_ptr;
    hash_table = selection->hash_table;
    aot_hash_size = selection->hash_size;
    aot_file_generation = selection->file_generation;
}

static TranslationBlock *aot_load_dispatch_selected(
        const AOTLoadIndexSelection *selection, CPUState *cpu, vaddr pc,
        uint64_t cs_base, uint32_t flags, int cflags, ENV *lsenv)
{
#ifdef CONFIG_LATX_TU
    if (selection->use_tu) {
        return load_aot_tu(cpu, pc, cs_base, flags, cflags, lsenv);
    }
#else
    (void)selection;
#endif
    return load_aot_tb(cpu, pc, cs_base, flags, cflags, lsenv);
}

TranslationBlock *load_aot(CPUState *cpu, vaddr pc, uint64_t cs_base,
        uint32_t flags, int cflags, ENV *lsenv)
{
    AOTLoadIndexSelection selection;

    if(!option_sys_aot || !option_aot_load) {
        return NULL;
    }
#ifdef CONFIG_SOFTMMU
    if (hamt_enabled() && !can_do_hamt(lsenv->cpu_state)) {
        return NULL;
    }
#endif

    if (!aot_load_select_index(flags, &selection)) {
        return NULL;
    }
    aot_load_bind_index(&selection);
    return aot_load_dispatch_selected(&selection, cpu, pc, cs_base, flags,
            cflags, lsenv);
}
