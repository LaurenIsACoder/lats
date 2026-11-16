/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sys-aot.h"
#include "sysemu/latx-aot.h"
#include "sys-aot-internal.h"
#include "sys-aot-merge.h"
#include "../tb-record.h"
#include "accel/tcg//tb-hash.h"
/* #include "exec/helper-head.h" */
/* #include "helper.h" */
#include "tcg/tcg.h"
#include "translate.h"
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
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
#include "lsenv.h"
#include "qemu/typedefs.h"
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

int in_aot_translate;
static bool aot_initialized;
#if DEBUG_AOT
static uint64_t all_time;
uint64_t cpu_loop_exec_tb_time;
uint64_t loop_time;
uint64_t exec_time;
uint64_t exec_count;
char *hash_file_name;
// 0: none, 1: gen, 2:use
int out_tb_file_control;
uint64_t out_tb_file_tb_count;
uint64_t file_time_counter;
uint64_t find_time;
uint64_t cmp_tb_count;
uint64_t one_ins_tb;
extern uint64_t load_page_count;

extern int load_core_tb_count, load_user_tb_count;
#endif

char *aot_file_path;
char *aot_file_lock;
FILE* out_tb_file;

__thread void* in_tb_ptr;
__thread AOTPageIndexEntry *hash_table;
__thread uint32_t aot_hash_size = 0;
__thread uint32_t aot_file_generation;

AOTPublishedFile *core_aot_file, *user_aot_file;

static pthread_mutex_t out_file_lock;

static void aot_file_init(void)
{
    aot_file_path = malloc(PATH_MAX * sizeof(char));
    if (!aot_file_path) {
        fprintf(stderr, "alloc aot_file_path fail\n");
        exit(1);
    }
    char *home __attribute__((unused)) = getenv("HOME");
#if DEBUG_AOT
    snprintf(aot_file_path, PATH_MAX, "%s", "/tmp/latx-sys-aot/");
#else
    if (likely(home)) {
        snprintf(aot_file_path, PATH_MAX, "%s%s", home, "/.cache/latx/sys/");
    } else {
        snprintf(aot_file_path, PATH_MAX, "%s", "/.cache/latx/sys/");
    }
#endif
    aot_file_path[PATH_MAX - 1] = 0;
    for (int i = 0; i < strlen(aot_file_path); i++) {
        if (aot_file_path[i] == '/') {
            int ret = 0;
            aot_file_path[i] = '\0';
            if (strlen(aot_file_path) && access(aot_file_path, R_OK) < 0) {
                ret = mkdir(aot_file_path, 0777);
                if (ret) {
                    fprintf(stderr, "create aot dir fail\n");
                }
            }
            aot_file_path[i] = '/';
        }
    }

    core_aot_file = NULL;
    user_aot_file = NULL;

    bool loaded_any = false;
    core_aot_file = load_aot_file("sys-aot-core", AOT_CORE);
    if (!core_aot_file) {
        char tmpp[PATH_MAX];
        bool file_exists;
        snprintf(tmpp, PATH_MAX, "%ssys-aot-core", aot_file_path);
        file_exists = access(tmpp, F_OK) == 0;
        /* A missing cache file is normal; only report it with verbose. */
        if (file_exists || option_verbose) {
            fprintf(stderr, "load sys-aot-core fail (exists=%d)\n",
                    file_exists);
        }
    } else {
        loaded_any = true;
    }

    user_aot_file = load_aot_file("sys-aot-user", AOT_USER);
    if (!user_aot_file) {
        char tmpp[PATH_MAX];
        bool file_exists;
        snprintf(tmpp, PATH_MAX, "%ssys-aot-user", aot_file_path);
        file_exists = access(tmpp, F_OK) == 0;
        if (file_exists || option_verbose) {
            fprintf(stderr, "load sys-aot-user fail (exists=%d)\n",
                    file_exists);
        }
    } else {
        loaded_any = true;
    }

    if (!loaded_any) {
    }
    //user_hash_table = user_tb_ptr + file_size - sizeof(aot_tail) - HASH_TABLE_SIZE;
}

void aot_init(void)
{
#if DEBUG_AOT
    all_time = get_tsc();
#endif
    aot_perf_init_from_env();
    aot_perf_emit_run_context(option_sys_aot, option_aot_load, option_aot_save);
    if (!option_sys_aot) {
        return;
    }
    aot_file_init();
    aot_reloc_init();
    pthread_mutex_init(&out_file_lock, NULL);
    aot_initialized = true;
    aot_merger_start();
}

void aot_shutdown(void)
{
    static bool started;

    if (started) {
        return;
    }
    if (option_aot_verify) {
        return;
    }
    /* Skip output in offline mode */
    if (latx_offline_mode_active) {
        return;
    }
    /* Help/version and failed option parsing can exit before aot_init(). */
    if (!aot_initialized || !option_sys_aot || !option_aot_save) {
        return;
    }

    started = true;
    aot_verbose_log("[AOT][Shutdown] Starting %s save/merge\n",
            option_wait_aot ? "synchronous" : "asynchronous");
    aot_save_on_shutdown();
#if DEBUG_AOT
    all_time = get_tsc() - all_time;
#endif

#if defined(CONFIG_SOFTMMU) && defined(CONFIG_LATX_TU)
    latxs_external_tu_stub_summary();
#endif
#if DEBUG_AOT
    fprintf(stderr, "[AOT][Summary] Loaded: %ld pages, %d TBs "
            "(Core: %d, User: %d)\n",
            load_page_count, load_core_tb_count + load_user_tb_count,
            load_core_tb_count, load_user_tb_count);
    fprintf(stderr, "[AOT][Summary] Elapsed (including shutdown): %.3fs\n",
            (double)all_time / 100000000);
#endif
    if (option_tb_record) {
        tb_record_fini();
    }
}

static void __attribute__((destructor)) sys_dump_out_tb_file_fini(void)
{
    aot_perf_emit_summary();
    /* Fallback for exits which do not pass through qemu_cleanup(). */
    aot_shutdown();
}

struct tb_desc {
    target_ulong pc;
    target_ulong cs_base;
    CPUArchState *env;
    tb_page_addr_t phys_page1;
    uint32_t flags;
    uint32_t cflags;
    /* uint32_t trace_vcpu_dstate; */
};
