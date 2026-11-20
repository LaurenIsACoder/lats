/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sys-aot.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "block/aio.h"
#include "sys-aot-internal.h"
#include "sys-aot-merge.h"
#include "sys-aot-common.h"
#include "sys-aot-format.h"
#include "sys-aot-tu-codec.h"
#include "sys-aot-tu-merge.h"
#include "accel/tcg//tb-hash.h"
#include "tcg/tcg.h"
#include "translate.h"
#include <signal.h>
#include <pthread.h>
#include <limits.h>
#include <dirent.h>
#include <syscall.h>
#include "latx-options.h"
#include "latx-misc.h"
#include "accel/tcg/internal.h"
#include "accel/tcg/tb-context.h"
#include "include/exec/ram_addr.h"
#include "include/tcg/insn-start-words.h"
#include "lsenv.h"
#include "qemu/typedefs.h"
#include "latx-ras.h"
#include "latx-tu.h"
#include "file_ctx.h"
#include "include/tcg/tcg.h"
#include "tcg/tcg-internal.h"
#include "include/qemu/cacheflush.h"
#include "include/exec/cputlb.h"
#include "include/hamt-ptw.h"
#include "include/hamt-fake-fork.h"
#include "include/hamt-options.h"
#include "include/exec/tb-flush.h"

#ifndef CONFIG_SOFTMMU
static void *qemu_ram_addr_to_host(uint64_t addr)
{
    fprintf(stderr, "%s:%d user mode not support aot!\n", __func__, __LINE__);
    exit(-1);
    return NULL;
}
#endif

typedef struct AOTSaveContext {
    /* Owned page metadata for one output AOT file. */
    page_info_t *page_info_list;
    uint32_t page_info_capacity;
    uint32_t page_info_count;
    uint64_t output_tb_count;
    int work_busy;
    int all_tb_num;
    int jit_core_tb_num;
    int jit_user_tb_num;
    int tb_num;
    int core_tb_num;
    int user_tb_num;
    int record_index;
    int in_ram;
    int bios;
    int invalid;
    int over_page;
    int page_num;
    int skip_page_num;
    /*
     * tb_vector owns the collected TB array. core_tb_vector and
     * user_tb_vector are slices into tb_vector and are never freed directly.
     */
    TranslationBlock **tb_vector;
    TranslationBlock **core_tb_vector;
    TranslationBlock **user_tb_vector;
} AOTSaveContext;

typedef struct AOTPageWriter {
    uint8_t *guest_code;
    uintptr_t guest_code_ptr;
    off_t file_offset;
    page_head head;
} AOTPageWriter;

static void aot_page_writer_cleanup(AOTPageWriter *page);

typedef struct AOTFlushSaveTrigger {
    int mtcc_id;
    bool has_core;
    bool has_user;
} AOTFlushSaveTrigger;

typedef struct AOTFlushSaveJob {
    AOTSaveContext *ctx;
    AOTFlushSaveTrigger trigger;
    bool merge_only;
    bool shutdown;
    /* Worker owns the write end until exit; parent owns the read end. */
    int completion_read_fd;
    int completion_write_fd;
    /* Final worker inherits the preceding runtime worker's read end. */
    int previous_save_fd;
} AOTFlushSaveJob;

typedef struct AOTRuntimeSaveState {
    pid_t child_pid;
    int completion_fd;
    pthread_mutex_t wait_mutex;
} AOTRuntimeSaveState;

typedef struct AOTSaveState {
    AOTSaveContext ctx;
    AOTRuntimeSaveState runtime;
} AOTSaveState;

enum {
    AOT_SYNC_SAVE_MIN_TB = 1000,
    AOT_FLUSH_SAVE_MIN_TB = 5000,
    AOT_CHILD_EXIT_BASE = 64,
    AOT_CHILD_MERGE_FAILED = 4,
};

static AOTSaveState aot_save_state = {
    .runtime = {
        .child_pid = 0,
        .completion_fd = -1,
        .wait_mutex = PTHREAD_MUTEX_INITIALIZER,
    },
};

/* Set only in the fork child; the parent observes its own COW copy as false. */
static bool aot_runtime_save_child;
static int aot_merge_shutting_down;

static AOTSaveContext *aot_save_state_context(void)
{
    return &aot_save_state.ctx;
}

static AOTRuntimeSaveState *aot_save_state_runtime(void)
{
    return &aot_save_state.runtime;
}

static void aot_save_fatal(void)
{
    _exit(EXIT_FAILURE);
}

static void aot_save_context_reset_tb(AOTSaveContext *ctx)
{
    ctx->all_tb_num = 0;
    ctx->jit_core_tb_num = 0;
    ctx->jit_user_tb_num = 0;
    ctx->tb_num = 0;
    ctx->core_tb_num = 0;
    ctx->user_tb_num = 0;
    ctx->record_index = 0;
    ctx->tb_vector = NULL;
    ctx->core_tb_vector = NULL;
    ctx->user_tb_vector = NULL;
}

static void aot_save_context_free_tb(AOTSaveContext *ctx)
{
    /* core/user vectors alias tb_vector and are invalidated with it. */
    free(ctx->tb_vector);
    aot_save_context_reset_tb(ctx);
}

static void aot_save_context_reset_page_info(AOTSaveContext *ctx)
{
    ctx->page_info_list = NULL;
    ctx->page_info_capacity = 0;
    ctx->page_info_count = 0;
    ctx->output_tb_count = 0;
}

static void aot_save_context_free_page_info(AOTSaveContext *ctx)
{
    free(ctx->page_info_list);
    aot_save_context_reset_page_info(ctx);
}

static bool aot_save_context_reserve_page_info(AOTSaveContext *ctx)
{
    if (ctx->page_info_count < ctx->page_info_capacity) {
        return true;
    }

    uint32_t new_capacity = ctx->page_info_capacity ?
        ctx->page_info_capacity * 2 : 1024;
    page_info_t *new_page_info_list = realloc(ctx->page_info_list,
            new_capacity * sizeof(page_info_t));
    if (!new_page_info_list) {
        fprintf(stderr, "realloc page_info_list failed\n");
        return false;
    }

    ctx->page_info_list = new_page_info_list;
    ctx->page_info_capacity = new_capacity;
    return true;
}

__thread CPUState *offline_cpu;
static CPUState *global_offline_cpu;
static ENV *global_lsenv;
static TUControl *global_tu_data;
static int tb_cmp(const void *a, const void *b)
{
    TranslationBlock *pa = *(TranslationBlock **)a;
    TranslationBlock *pb = *(TranslationBlock **)b;
    if ((pa->flags & HF_CPL_MASK) > (pb->flags & HF_CPL_MASK)) {
        return 1;
    } else if ((pa->flags & HF_CPL_MASK) < (pb->flags & HF_CPL_MASK)) {
        return -1;
    }
    uint64_t addr1 = tb_page_addr0(pa);
    uint64_t addr2 = tb_page_addr0(pb);
    if (addr1 > addr2) {
        return 1;
    } else if (addr1 < addr2) {
        return -1;
    }
    uint64_t pc1 = pa->pc & ~TARGET_PAGE_MASK;
    uint64_t pc2 = pb->pc & ~TARGET_PAGE_MASK;
    if (pc1 > pc2) {
        return 1;
    } else if (pc1 < pc2) {
        return -1;
    }
    if (pa->cs_base > pb->cs_base) {
        return 1;
    } else if (pa->cs_base < pb->cs_base) {
        return -1;
    }
    if (pa->flags > pb->flags) {
        return 1;
    } else if (pa->flags < pb->flags) {
        return -1;
    }
    uint32_t a_cflags = pa->cflags;
    uint32_t b_cflags = pb->cflags;
    return a_cflags > b_cflags ? 1 : (a_cflags < b_cflags ? -1 : 0);
}

static inline bool not_save(TranslationBlock *tb)
{
#ifdef CONFIG_SOFTMMU
    tb_page_addr_t page_addr = tb_page_addr0(tb);

    if (page_addr == -1
            || tb_page_addr1(tb) != -1
            || (hamt_enabled() && !(tb->bool_flags & CAN_DO_HAMT))
            || (tb->bool_flags & IS_AOT_TB)
            || (tb->cflags & CF_INVALID)) {
        return true;
    }

    if (!aot_host_pc_in_main_ram(qemu_ram_addr_to_host(page_addr))) {
        return true;
    }
#endif
    return false;
}

static void do_tb_record(void *p, uint32_t hash, void *userp)
{
    AOTSaveContext *ctx = userp;
    TranslationBlock *tb = (TranslationBlock *)p;
    if (not_save(tb)) {
        return;
    }
    assert(ctx->record_index < ctx->tb_num);
    if (ctx->record_index >= ctx->tb_num) {
       fprintf(stderr, "record_index >= tb_num !!!!!\n");
       exit(-1);
    }
    ctx->tb_vector[ctx->record_index++] = (TranslationBlock *)p;
}


static void do_tb_count(void *p, uint32_t hash, void *userp)
{
    AOTSaveContext *ctx = userp;
    TranslationBlock *tb = p;

    /* Count resident JIT TBs before save filtering and retranslation. */
    if (!(tb->bool_flags & IS_AOT_TB)) {
        if (tb->flags & HF_CPL_MASK) {
            ctx->jit_user_tb_num++;
        } else {
            ctx->jit_core_tb_num++;
        }
    }
    if (!not_save(tb)) {
        ctx->tb_num++;
    }
    ctx->all_tb_num++;
}

void print_tb_bytes(TranslationBlock *tb)
{
    /* fprintf(stderr, "print_tb_bytes ======= \n"); */
#ifdef CONFIG_SOFTMMU
    uintptr_t p;
    if (tb_page_addr0(tb) != -1) {
        aot_save_state_context()->in_ram++;
        fprintf(stderr, "tid %ld in_ram bytes ", syscall(SYS_gettid));
        p = (uintptr_t)qemu_ram_addr_to_host(tb_page_addr0(tb));
        fprintf(stderr, "p %lx\n", p);
        if (tb_page_addr1(tb) == -1) {
            for (int j = 0; j < tb->size; ++j) {
                fprintf(stderr, "%02x", *(uint8_t *)(p + j));
            }
        } else {
            int p0size = 0x1000 - (tb_page_addr0(tb) & 0xfff);
            for (int j = 0; j < p0size; ++j) {
                fprintf(stderr, "%02x", *(uint8_t *)(p + j));
            }
            p = (uintptr_t)qemu_ram_addr_to_host(tb_page_addr1(tb));
            for (int j = 0; j < (tb->size - p0size); ++j) {
                fprintf(stderr, "%02x", *(uint8_t *)(p + j));
            }
        }
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "invalid code page\n");
        /* bios++; */
    }
#endif
}

static size_t aot_qht_iter(struct qht *ht, qht_iter_func_t func,
                           AOTSaveContext *ctx, bool nonblocking)
{
    if (nonblocking) {
        return qht_iter_try(ht, func, ctx);
    }
    qht_iter(ht, func, ctx);
    return 0;
}

static bool get_tb(AOTSaveContext *ctx, int mtcc_id, bool nonblocking)
{
    size_t skipped_buckets = 0;

    aot_save_context_reset_tb(ctx);
    if (!tb_ctx.htable->map) {
        return true;
    }
    if (mtcc_id == -1) {
        skipped_buckets += aot_qht_iter(&tb_ctx.htable[0], do_tb_count,
                                        ctx, nonblocking);
        skipped_buckets += aot_qht_iter(&tb_ctx.htable[1], do_tb_count,
                                        ctx, nonblocking);
    } else {
        skipped_buckets += aot_qht_iter(&tb_ctx.htable[mtcc_id],
                                        do_tb_count, ctx, nonblocking);
    }

    aot_verbose_log("[AOT][Save] Collected: all=%d, eligible=%d, "
            "skipped_buckets=%zu\n", ctx->all_tb_num, ctx->tb_num,
            skipped_buckets);
    aot_verbose_log("[AOT][Save] Resident JIT: %d TBs (Core: %d, User: %d)\n",
            ctx->jit_core_tb_num + ctx->jit_user_tb_num,
            ctx->jit_core_tb_num, ctx->jit_user_tb_num);
    ctx->tb_vector =
        (TranslationBlock **)malloc(ctx->tb_num * sizeof(TranslationBlock *));
    if (!ctx->tb_vector && ctx->tb_num) {
        fprintf(stderr, "[AOT] TB Collection: alloc vector failed\n");
        aot_save_context_reset_tb(ctx);
        return false;
    }
    if (mtcc_id == -1) {
        aot_qht_iter(&tb_ctx.htable[0], do_tb_record, ctx, nonblocking);
        aot_qht_iter(&tb_ctx.htable[1], do_tb_record, ctx, nonblocking);
    } else {
        aot_qht_iter(&tb_ctx.htable[mtcc_id], do_tb_record, ctx,
                     nonblocking);
    }
    if (ctx->tb_num == 0) {
        aot_save_context_free_tb(ctx);
        return true;
    }

    qsort(ctx->tb_vector, ctx->tb_num, sizeof(TranslationBlock *), tb_cmp);

    TranslationBlock *pre_tb = ctx->tb_vector[0];
    int last_id = 1;
    for (int i = 1; i < ctx->tb_num; i++) {
        TranslationBlock *tb = ctx->tb_vector[i];
        if (
                tb->hva == pre_tb->hva
                && ((tb->pc & ~TARGET_PAGE_MASK) == (pre_tb->pc & ~TARGET_PAGE_MASK))
                && (tb->cs_base == pre_tb->cs_base)
                && (tb->flags == pre_tb->flags)
                && (tb->cflags == pre_tb->cflags)) {
            continue;
        }
        ctx->tb_vector[last_id++] = tb;
        if (!(tb->flags & HF_CPL_MASK)) {
            ctx->core_tb_num = last_id;
            if ((pre_tb->flags & HF_CPL_MASK)) {
                fprintf(stderr, "sort error in get_tb()\n");
            }
        }
        pre_tb = tb;
    }
    ctx->tb_num = last_id;
    ctx->user_tb_num = ctx->tb_num - ctx->core_tb_num;
    ctx->core_tb_vector = ctx->tb_vector;
    ctx->user_tb_vector = ctx->tb_vector + ctx->core_tb_num;

    aot_verbose_log("[AOT][Save] Selected: %d TBs (Core: %d, User: %d)\n",
            ctx->tb_num, ctx->core_tb_num, ctx->user_tb_num);
    if (option_verbose) {
        fprintf(stderr, "[AOT][Save] Stats: in_ram=%d bios=%d "
                "invalid=%d over_page=%d\n",
                ctx->in_ram, ctx->bios, ctx->invalid, ctx->over_page);
    }
    return true;
}

static bool aot_save_context_has_min_tb(AOTSaveContext *ctx, int min_tb_count)
{
    if (ctx->tb_num >= min_tb_count) {
        return true;
    }

    fprintf(stderr, "[AOT] Skip: TB count %d too few (need >= %d)\n",
            ctx->tb_num, min_tb_count);
    return false;
}

static bool aot_save_context_select_flush_outputs(AOTSaveContext *ctx,
        bool *has_core, bool *has_user)
{
    *has_core = (ctx->core_tb_num >= AOT_FLUSH_SAVE_MIN_TB);
    *has_user = (ctx->user_tb_num >= AOT_FLUSH_SAVE_MIN_TB);
    return *has_core || *has_user;
}

static void aot_save_context_begin_translate(void)
{
    in_aot_translate = true;
}

static void aot_save_context_end_translate(AOTSaveContext *ctx)
{
    aot_save_context_free_tb(ctx);
    in_aot_translate = false;
}


static bool aot_save_append_page_info(AOTSaveContext *ctx,
        uint32_t page_hash, uint64_t offset_in_file)
{
    if (!aot_save_context_reserve_page_info(ctx)) {
        return false;
    }

    ctx->page_info_list[ctx->page_info_count].page_hash = page_hash;
    ctx->page_info_list[ctx->page_info_count].file_offset = offset_in_file;
    ctx->page_info_count++;
    return true;
}

static void aot_reset_translation_buffers(void);

static void aot_prepare_save_cpu(void)
{
    current_cpu = offline_cpu;
    CPUArchState *env = offline_cpu->env_ptr;
    env->hflags |= HF_CPL_MASK;
#ifdef CONFIG_SOFTMMU
    uint32_t mtcc_id = curr_mtcc(env);
    unlock_all_page(mtcc_id);
#endif
    /* Reset the CPL3/MTCC context that AOT retranslation will use. */
    aot_reset_translation_buffers();
}

static void aot_reset_translation_buffers(void)
{
    CPUArchState *env;
    uint32_t mtcc_id;
    TCGContext *s;

    g_assert(in_aot_translate);
    g_assert(offline_cpu);

    env = offline_cpu->env_ptr;
    mtcc_id = curr_mtcc(env);
    s = tcg_ctx_mtcc[mtcc_id];
    g_assert(s && s->code_gen_buffer);

    /*
     * Save runs either in the fork child or at process shutdown, after guest
     * execution has stopped.  tb_flush_mtcc() is not a synchronous allocator
     * reset: an MTTCG CPU outside exclusive context queues work on a vCPU
     * which no longer runs at shutdown.  Rewind our scratch region directly.
     * Do not reset the TB allocator: collected templates still live there.
     * Callers only reuse generated output after it has been serialized.
     */
    tcg_region_rewind_current(s);
    s->data_gen_ptr = NULL;
    region_reset_search(mtcc_id);
}

void aot_prepare_merge_translation(void)
{
    g_assert(aot_tu_merge_cpu && in_aot_translate);
    aot_prepare_save_cpu();
    clear_tu_rel_table();
}

void aot_reset_merge_translation(void)
{
    clear_tu_rel_table();
    aot_reset_translation_buffers();
    if (option_split_tb) {
        region_reset_tb(curr_mtcc(offline_cpu->env_ptr));
    }
}

static void aot_reclaim_translation_buffers(void)
{
    aot_reset_translation_buffers();
}

static bool aot_page_writer_begin(AOTSaveContext *ctx, AOTPageWriter *page,
        uint64_t page_addr, int tb_count)
{
    memset(page, 0, sizeof(*page));
    page->guest_code = malloc(TARGET_PAGE_SIZE);
    if (!page->guest_code) {
        return false;
    }

    page->guest_code_ptr = (uintptr_t)qemu_ram_addr_to_host(page_addr);
    memcpy((void *)page->guest_code, (void *)page->guest_code_ptr,
            TARGET_PAGE_SIZE);

    page->head.page_magic = AOT_PAGE_MAGIC;
    page->head.page_hash = get_page_hash((uint32_t *)page->guest_code);
    page->head.tb_count = tb_count;
    page->head.curr_page_sum_size = TARGET_PAGE_SIZE + sizeof(page_head);

    page->file_offset = ftello(out_tb_file);
    if (page->file_offset < 0 ||
            fwrite(&page->head, sizeof(page->head), 1, out_tb_file) != 1 ||
            fwrite(page->guest_code, TARGET_PAGE_SIZE, 1, out_tb_file) != 1) {
        fprintf(stderr, "[AOT] Save: failed to write page prefix\n");
        aot_page_writer_cleanup(page);
        return false;
    }
    if (!aot_save_append_page_info(ctx, page->head.page_hash,
                                  page->file_offset)) {
        aot_page_writer_cleanup(page);
        return false;
    }
    return true;
}

static bool aot_page_writer_finish(AOTPageWriter *page)
{
    off_t page_end = page->file_offset + page->head.curr_page_sum_size;

    if (ferror(out_tb_file) ||
            fseeko(out_tb_file, page->file_offset, SEEK_SET) != 0 ||
            fwrite(&page->head, sizeof(page->head), 1, out_tb_file) != 1 ||
            fseeko(out_tb_file, page_end, SEEK_SET) != 0) {
        fprintf(stderr, "[AOT] Save: failed to finalize page\n");
        return false;
    }
    return true;
}

static void aot_page_writer_cleanup(AOTPageWriter *page)
{
    free(page->guest_code);
    page->guest_code = NULL;
}

static bool aot_save_write(const void *ptr, size_t size, size_t count)
{
    if (!size || !count) {
        return true;
    }

    if (fwrite(ptr, size, count, out_tb_file) != count) {
        fprintf(stderr, "[AOT] Save: file write failed\n");
        return false;
    }
    return true;
}

static inline bool save_page(AOTSaveContext *ctx,
        TranslationBlock **curr_tb_vector, int tb_count, uint64_t page_addr)
{
    if (!out_tb_file) {
        return false;
    }

    uint32_t tb_magic = AOT_TB_MAGIC;
    AOTPageWriter page;

    if (!aot_page_writer_begin(ctx, &page, page_addr, tb_count)) {
        return false;
    }

    uint32_t all_tb_size;
    uint32_t rel_count;
    uint32_t sum_size;
    TranslationBlock *tb;
    for (int i = 0; i < tb_count; i++) {
        tb = curr_tb_vector[i];
        remove_misc_flags(tb, MF_LINK0_MASK | MF_LINK1_MASK | MF_LINK_STATUS);
        latxs_ras_unlink(tb);
#if DEBUG_AOT
        ++out_tb_file_tb_count;
#endif
        if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
            latx_tb_reset_jmp_target(tb, 0);
        }
        if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
            latx_tb_reset_jmp_target(tb, 1);
        }

        if (option_split_search) {
            all_tb_size = (tb->ir2_num << 2) + (tb->icount << 2) + sizeof(aot_block);
        } else {
            all_tb_size = (tb->ir2_num << 2) + tb->search_size + sizeof(aot_block);
        }

        if (option_pushpop) {
            all_tb_size += tb->icount << 2;
        }

        aot_block aot_tb = { 0 };
        aot_encode_block(&aot_tb, tb);

        rel_count = 0;
        if (TB_REL_START(tb) > -1) {
            rel_count = TB_REL_END(tb) - TB_REL_START(tb) + 1;
        }
        sum_size = 16 + all_tb_size + rel_count * sizeof(aot_rel);
        page.head.curr_page_sum_size += sum_size;
        if (!aot_save_write(&tb_magic, 4, 1) ||
            !aot_save_write(&sum_size, 4, 1) ||
            !aot_save_write(&all_tb_size, 4, 1) ||
            !aot_save_write(&rel_count, 4, 1) ||
            !aot_save_write(&aot_tb, sizeof(aot_block), 1)) {
            goto fail;
        }
        if (option_split_search) {
            if (!aot_save_write(tb->tc.ptr, tb->ir2_num << 2, 1) ||
                !aot_save_write(tb->search_ptr, tb->icount << 2, 1)) {
                goto fail;
            }
        } else {
            if (!aot_save_write(tb->tc.ptr,
                                all_tb_size - sizeof(aot_block), 1)) {
                goto fail;
            }
        }
        if (option_pushpop &&
            !aot_save_write(tb_opt_ptr(tb), tb->icount << 2, 1)) {
            goto fail;
        }
        if (rel_count != 0 &&
            !aot_save_write(&rel_table[TB_REL_START(tb)],
                            sizeof(aot_rel) * rel_count, 1)) {
            goto fail;
        }
        if (0) {
            fprintf(stderr, "save tb  pc %lx tb_size %d  guest_code_size %d  rel_size %ld rel_count %d sum %ld\n",
                    tb->pc, all_tb_size, tb->size, sizeof(aot_rel) * rel_count,
                    rel_count,
                    16 + all_tb_size  + sizeof(aot_rel) * rel_count);
            fprintf(stderr, "ir2 size %d icount %d\n", tb->ir2_num << 2, tb->icount);
            fprintf(stderr, "  ==================\n");
            fprintf(stderr, "|| cs_base:%x, pc:%lx\n", (uint32_t)tb->cs_base, tb->pc);
            fprintf(stderr, "|| flags:%x, cflags:%x, trace_vcpu_dstate:%x\n", tb->flags, tb->cflags, 0);
            fprintf(stderr, "|| size:%x, icount:%x\n", tb->size, tb->icount);
            fprintf(stderr, "|| page_next[0]:%lx, page_next[1]:%lx\n", tb->page_next[0], tb->page_next[1]);
            fprintf(stderr, "|| page_addr[0]:%lx, page_addr[1]:%lx\n", tb->page_addr[0], tb->page_addr[1]);
            fprintf(stderr, "|| code: ");
            fprintf(stderr, "\n");
            fprintf(stderr, "|| ir2: ");
            uint32_t *ir2_vec = (uint32_t *)tb->tc.ptr;
            for (int i = 0; i < tb->ir2_num; i++) {
                fprintf(stderr, "i %d %x", i, ir2_vec[i]);
            }
            fprintf(stderr, "  ==================\n");
        }
    }

    bool page_ok = aot_page_writer_finish(&page);
    if (page_ok) {
        ctx->output_tb_count += page.head.tb_count;
    }
    aot_page_writer_cleanup(&page);
    return page_ok;

fail:
    aot_page_writer_cleanup(&page);
    return false;
}

static bool save_tb_by_page(AOTSaveContext *ctx,
        TranslationBlock **curr_tb_vector, int curr_tb_num, uint32_t aot_mode)
{
    if (!curr_tb_num) {
        return true;
    }

    aot_prepare_save_cpu();

    int l = 0, r = 1;
    uint64_t curr_hva = tb_page_addr0(curr_tb_vector[0]) & TARGET_PAGE_MASK;

    while (r < curr_tb_num) {
        if (curr_tb_vector[r]->hva != curr_hva) {
            if (!save_page(ctx, curr_tb_vector + l, r - l, curr_hva)) {
                return false;
            }
            l = r;
            curr_hva = tb_page_addr0(curr_tb_vector[r]) & TARGET_PAGE_MASK;
        }
        r++;
    }
    return save_page(ctx, curr_tb_vector + l, r - l, curr_hva);
}

#ifdef CONFIG_LATX_TU
static int aot_tu_page_cmp(const void *ap, const void *bp)
{
    const aot_tu_page *a = ap;
    const aot_tu_page *b = bp;

    return a->pc_delta < b->pc_delta ? -1 : a->pc_delta > b->pc_delta;
}

typedef struct AOTTUSavePage {
    target_ulong guest_page_addr;
    tb_page_addr_t ram_page_addr;
    uint64_t page_fingerprint;
} AOTTUSavePage;

typedef enum AOTTUMappingResult {
    AOT_TU_MAPPING_OK,
    AOT_TU_MAPPING_PAGE_LIMIT,
    AOT_TU_MAPPING_CONFLICT,
    AOT_TU_MAPPING_INVALID_RAM,
    AOT_TU_MAPPING_RESULT_COUNT,
} AOTTUMappingResult;

typedef struct AOTTUSaveRef {
    tb_page_addr_t ram_page_addr;
    target_ulong guest_page_addr;
    uint64_t page_fingerprint;
    uint64_t tu_id;
    int64_t anchor_delta;
    size_t tu_index;
} AOTTUSaveRef;

typedef struct AOTTUSpoolRecord {
    off_t file_offset;
    uint32_t size;
    uint32_t tb_count;
    target_ulong anchor_guest_page_addr;
    uint64_t tu_id;
} AOTTUSpoolRecord;

typedef struct AOTCrossPageSpool {
    FILE *file;
    uint8_t *copy_buffer;
    AOTTUSpoolRecord *records;
    size_t record_count;
    size_t record_capacity;
    AOTTUSaveRef *refs;
    size_t ref_count;
    size_t ref_capacity;
    void *code_rewind;
    uint32_t mtcc_id;
    int mapping_rejects[AOT_TU_MAPPING_RESULT_COUNT];
} AOTCrossPageSpool;

#define AOT_TU_COPY_BUFFER_SIZE (64 * 1024)

static int aot_save_ref_cmp(const void *ap, const void *bp)
{
    const AOTTUSaveRef *a = ap;
    const AOTTUSaveRef *b = bp;
    int code_cmp;

    if (a->page_fingerprint != b->page_fingerprint) {
        return a->page_fingerprint < b->page_fingerprint ? -1 : 1;
    }
    code_cmp = memcmp(qemu_ram_addr_to_host(a->ram_page_addr),
                      qemu_ram_addr_to_host(b->ram_page_addr),
                      TARGET_PAGE_SIZE);
    if (code_cmp) {
        return code_cmp;
    }
    if (a->tu_id != b->tu_id) {
        return a->tu_id < b->tu_id ? -1 : 1;
    }
    if (a->anchor_delta != b->anchor_delta) {
        return a->anchor_delta < b->anchor_delta ? -1 : 1;
    }

    if (a->ram_page_addr != b->ram_page_addr) {
        return a->ram_page_addr < b->ram_page_addr ? -1 : 1;
    }
    if (a->guest_page_addr != b->guest_page_addr) {
        return a->guest_page_addr < b->guest_page_addr ? -1 : 1;
    }
    return a->tu_index < b->tu_index ? -1 : a->tu_index > b->tu_index;
}

static bool aot_save_refs_same_page(const AOTTUSaveRef *a,
                                    const AOTTUSaveRef *b)
{
    return a->page_fingerprint == b->page_fingerprint &&
           !memcmp(qemu_ram_addr_to_host(a->ram_page_addr),
                   qemu_ram_addr_to_host(b->ram_page_addr), TARGET_PAGE_SIZE);
}

static bool aot_save_refs_same_record(const AOTTUSaveRef *a,
                                      const AOTTUSaveRef *b)
{
    return a->tu_id == b->tu_id && a->anchor_delta == b->anchor_delta;
}

static bool aot_cross_page_reserve(void **array, size_t element_size,
                                   size_t *capacity, size_t count)
{
    size_t new_capacity = *capacity ? *capacity : 1024;

    if (count <= *capacity) {
        return true;
    }
    while (new_capacity < count) {
        if (new_capacity > SIZE_MAX / 2) {
            return false;
        }
        new_capacity *= 2;
    }
    void *new_array = g_try_realloc_n(*array, new_capacity, element_size);

    if (!new_array) {
        return false;
    }
    *array = new_array;
    *capacity = new_capacity;
    return true;
}

static AOTTUMappingResult aot_collect_tu_mappings(TranslationBlock **tb_vector,
                                    int tb_count,
                                    AOTTUSavePage *mappings,
                                    uint32_t *page_count)
{
    uint32_t mapping_count = 0;

    for (int tb_index = 0; tb_index < tb_count; tb_index++) {
        target_ulong guest_page_addr[2] = {
            tb_vector[tb_index]->pc & TARGET_PAGE_MASK,
            (tb_vector[tb_index]->pc & TARGET_PAGE_MASK) + TARGET_PAGE_SIZE,
        };
        tb_page_addr_t ram_page_addr[2] = {
            tb_page_addr0(tb_vector[tb_index]) & TARGET_PAGE_MASK,
            tb_page_addr1(tb_vector[tb_index]) & TARGET_PAGE_MASK,
        };
        int tb_page_count =
            tb_page_addr1(tb_vector[tb_index]) == (tb_page_addr_t)-1 ? 1 : 2;

        for (int page_index = 0; page_index < tb_page_count; page_index++) {
            uint32_t mapping_index;

            for (mapping_index = 0; mapping_index < mapping_count;
                    mapping_index++) {
                if (mappings[mapping_index].guest_page_addr ==
                        guest_page_addr[page_index]) {
                    if (mappings[mapping_index].ram_page_addr !=
                            ram_page_addr[page_index]) {
                        void *recorded_page_host = qemu_ram_addr_to_host(
                            mappings[mapping_index].ram_page_addr);
                        void *candidate_page_host = qemu_ram_addr_to_host(
                            ram_page_addr[page_index]);

                        if (!recorded_page_host || !candidate_page_host ||
                                !aot_host_pc_in_main_ram(recorded_page_host) ||
                                !aot_host_pc_in_main_ram(candidate_page_host)) {
                            return AOT_TU_MAPPING_INVALID_RAM;
                        }
                        /*
                         * A physical alias is compatible only if the entire
                         * page matches. Keep the first page as the canonical
                         * save reference; the loader resolves and locks the
                         * current physical mapping for this virtual page.
                         * Do not accept a fingerprint match without checking
                         * bytes, or rewrite live/template TB page addresses.
                         */
                        if (memcmp(recorded_page_host, candidate_page_host,
                                   TARGET_PAGE_SIZE)) {
                            return AOT_TU_MAPPING_CONFLICT;
                        }
                    }
                    break;
                }
            }
            if (mapping_index != mapping_count) {
                continue;
            }
            if (mapping_count == AOT_TU_MAX_PAGES) {
                return AOT_TU_MAPPING_PAGE_LIMIT;
            }
            void *page_host = qemu_ram_addr_to_host(ram_page_addr[page_index]);
            if (!page_host || !aot_host_pc_in_main_ram(page_host)) {
                return AOT_TU_MAPPING_INVALID_RAM;
            }
            mappings[mapping_count++] = (AOTTUSavePage) {
                .guest_page_addr = guest_page_addr[page_index],
                .ram_page_addr = ram_page_addr[page_index],
                .page_fingerprint = get_page_fingerprint(page_host),
            };
        }
    }
    *page_count = mapping_count;
    return mapping_count ? AOT_TU_MAPPING_OK : AOT_TU_MAPPING_INVALID_RAM;
}

static bool aot_encode_tu_pages(const AOTTUSavePage *mappings,
                                uint32_t mapping_count,
                                target_ulong first_page,
                                aot_tu_page *pages)
{
    if (!mapping_count || mapping_count > AOT_TU_MAX_PAGES) {
        return false;
    }
    for (uint32_t i = 0; i < mapping_count; i++) {
        pages[i] = (aot_tu_page) {
            .pc_delta = (int64_t)(mappings[i].guest_page_addr - first_page),
            .page_fingerprint = mappings[i].page_fingerprint,
        };
    }

    qsort(pages, mapping_count, sizeof(*pages), aot_tu_page_cmp);
    return true;
}

static bool save_one_tu(TranslationBlock **tb_vector, int tb_count,
                        const AOTTUSavePage *mappings,
                        uint32_t mapping_count, int *record_size,
                        uint64_t *tu_id)
{
    aot_tu_page pages[AOT_TU_MAX_PAGES];

    return aot_encode_tu_pages(mappings, mapping_count,
            tb_vector[0]->pc & TARGET_PAGE_MASK, pages) &&
        aot_encode_tu(out_tb_file, tb_vector, tb_count, pages, mapping_count,
                       record_size, tu_id);
}

static void aot_cross_page_reset_translation(AOTCrossPageSpool *spool)
{
    clear_tu_rel_table();
    qatomic_set(&tcg_ctx->code_gen_ptr, spool->code_rewind);
    tcg_ctx->data_gen_ptr = NULL;
    region_reset_search(spool->mtcc_id);
    if (option_split_tb) {
        region_reset_tb(spool->mtcc_id);
    }
}

static AOTCrossPageTUResult aot_spool_cross_page_tu(
        TranslationBlock **tb_vector, int tb_count, void *opaque)
{
    AOTCrossPageSpool *spool = opaque;
    AOTTUSavePage mappings[AOT_TU_MAX_PAGES];
    uint32_t mapping_count;
    FILE *final_file = out_tb_file;
    off_t start = -1;
    off_t end = -1;
    int serialized_size = 0;
    uint64_t tu_id = 0;
    AOTCrossPageTUResult result = AOT_TU_SAVE_ERROR;

    AOTTUMappingResult mapping_result = aot_collect_tu_mappings(
            tb_vector, tb_count, mappings, &mapping_count);

    if (mapping_result != AOT_TU_MAPPING_OK) {
        spool->mapping_rejects[mapping_result]++;
        result = AOT_TU_SAVE_RETRY;
        goto out;
    }
    if (spool->record_count == SIZE_MAX ||
            spool->ref_count > SIZE_MAX - mapping_count ||
            !aot_cross_page_reserve((void **)&spool->records,
                                    sizeof(*spool->records),
                                    &spool->record_capacity,
                                    spool->record_count + 1) ||
            !aot_cross_page_reserve((void **)&spool->refs,
                                    sizeof(*spool->refs),
                                    &spool->ref_capacity,
                                    spool->ref_count + mapping_count)) {
        fprintf(stderr, "[AOT] Save: alloc cross-page TU index failed\n");
        goto out;
    }

    out_tb_file = spool->file;
    start = ftello(spool->file);
    if (start >= 0 &&
            save_one_tu(tb_vector, tb_count, mappings, mapping_count,
                        &serialized_size, &tu_id)) {
        end = ftello(spool->file);
    }
    out_tb_file = final_file;
    if (start < 0 || end < start || end - start > UINT32_MAX ||
            serialized_size != end - start) {
        fprintf(stderr, "[AOT] Save: failed to spool cross-page TU\n");
        goto out;
    }

    size_t record_index = spool->record_count++;
    spool->records[record_index] = (AOTTUSpoolRecord) {
        .file_offset = start,
        .size = end - start,
        .tb_count = tb_count,
        .anchor_guest_page_addr = tb_vector[0]->pc & TARGET_PAGE_MASK,
        .tu_id = tu_id,
    };
    for (uint32_t i = 0; i < mapping_count; i++) {
        spool->refs[spool->ref_count++] = (AOTTUSaveRef) {
            .ram_page_addr = mappings[i].ram_page_addr,
            .guest_page_addr = mappings[i].guest_page_addr,
            .page_fingerprint = mappings[i].page_fingerprint,
            .tu_id = tu_id,
            .anchor_delta = (int64_t)(
                spool->records[record_index].anchor_guest_page_addr -
                mappings[i].guest_page_addr),
            .tu_index = record_index,
        };
    }
    result = AOT_TU_SAVE_OK;

out:
    out_tb_file = final_file;
    aot_cross_page_reset_translation(spool);
    return result;
}

static bool aot_copy_spooled_tu(AOTCrossPageSpool *spool,
                                const AOTTUSaveRef *ref,
                                int *curr_page_sum_size)
{
    const AOTTUSpoolRecord *record = &spool->records[ref->tu_index];
    aot_tu_head tu_head;
    size_t remaining;
    bool anchor_record = ref->anchor_delta == 0;
    uint32_t output_size = anchor_record ? record->size : sizeof(aot_tu_ref);

    if (record->size < sizeof(tu_head) || record->size > INT_MAX ||
            *curr_page_sum_size > INT_MAX - (int)output_size ||
            fseeko(spool->file, record->file_offset, SEEK_SET) != 0 ||
            fread(&tu_head, sizeof(tu_head), 1, spool->file) != 1 ||
            tu_head.tb_magic != AOT_TB_MAGIC ||
            tu_head.tu_sum_size != record->size ||
            tu_head.tb_count != record->tb_count ||
            tu_head.tu_id != record->tu_id || ref->tu_id != record->tu_id) {
        fprintf(stderr, "[AOT] Save: invalid spooled cross-page TU\n");
        return false;
    }

    if (!anchor_record) {
        aot_tu_ref tu_ref = {
            .ref_magic = AOT_TU_REF_MAGIC,
            .ref_size = sizeof(tu_ref),
            .tu_id = record->tu_id,
            .anchor_delta = ref->anchor_delta,
        };

        if (!aot_save_write(&tu_ref, sizeof(tu_ref), 1)) {
            return false;
        }
        *curr_page_sum_size += sizeof(tu_ref);
        return true;
    }

    if (!aot_save_write(&tu_head, sizeof(tu_head), 1)) {
        return false;
    }
    remaining = record->size - sizeof(tu_head);
    while (remaining) {
        size_t count = MIN(remaining, (size_t)AOT_TU_COPY_BUFFER_SIZE);

        if (fread(spool->copy_buffer, 1, count, spool->file) != count ||
                !aot_save_write(spool->copy_buffer, 1, count)) {
            fprintf(stderr, "[AOT] Save: failed to copy spooled TU\n");
            return false;
        }
        remaining -= count;
    }
    *curr_page_sum_size += record->size;
    return true;
}

static bool save_tu_page(TranslationBlock **curr_tb_vector, int tb_count,
                         int *curr_page_sum_size, int *saved_record_count,
                         int *saved_tb_count)
{
    int i = 0;

    *saved_record_count = 0;
    *saved_tb_count = 0;
    while (i < tb_count) {
        int j = i + 1;
        AOTTUSavePage mappings[AOT_TU_MAX_PAGES];
        uint32_t mapping_count;

        while (j < tb_count && !curr_tb_vector[j]->s_data->is_first_tb) {
            j++;
        }
        if (aot_collect_tu_mappings(curr_tb_vector + i, j - i, mappings,
                                    &mapping_count) != AOT_TU_MAPPING_OK ||
                mapping_count > 1) {
            i = j;
            continue;
        }
        if (!save_one_tu(curr_tb_vector + i, j - i, mappings, mapping_count,
                         curr_page_sum_size, NULL)) {
            return false;
        }
        (*saved_record_count)++;
        *saved_tb_count += j - i;
        i = j;
    }

    if (fflush(out_tb_file) == EOF) {
        fprintf(stderr, "[AOT] Save: TU flush failed\n");
        return false;
    }
    return true;
}

static bool deal_tu_page(AOTSaveContext *ctx,
        TranslationBlock **curr_tb_vector,
        int tb_count, uint32_t aot_mode)
{
    (void)aot_mode;
    clear_tu_rel_table();

    CPUArchState *env = offline_cpu->env_ptr;
    uint32_t mtcc_id = curr_mtcc(env);
    tcg_ctx = tcg_ctx_mtcc[mtcc_id];
    if (aot_runtime_save_child ||
            tcg_ctx->code_gen_highwater - tcg_ctx->code_gen_ptr <
            tcg_ctx->code_gen_buffer_size / 2) {
        /* The previous page has already been serialized at this boundary. */
        aot_reclaim_translation_buffers();
        tcg_ctx = tcg_ctx_mtcc[mtcc_id];
    } else {
        /*
         * Split search data and push/pop data share a buffer which is much
         * smaller than the code cache.  Code-cache pressure therefore cannot
         * be used to decide when that buffer needs reclaiming.  Everything
         * allocated for the previous page has already been serialized, so it
         * is safe to rewind the auxiliary buffer at every page boundary.
         */
        region_reset_search(mtcc_id);
    }

    assert(tb_page_addr0(curr_tb_vector[0]) != -1);

    AOTPageWriter page;
    uint64_t page_addr = tb_page_addr0(curr_tb_vector[0]) & TARGET_PAGE_MASK;
    if (!aot_page_writer_begin(ctx, &page, page_addr, 0)) {
        return false;
    }

    bool page_ok = false;
    TranslationBlock **translated_tb_vector = retranslate_by_tu(offline_cpu,
            curr_tb_vector[0]->pc & TARGET_PAGE_MASK, page.guest_code_ptr,
            curr_tb_vector, &tb_count);
    if (!translated_tb_vector) {
        fprintf(stderr, "[AOT] Save: TU retranslation failed\n");
        goto out;
    }

    int saved_record_count;
    int saved_tb_count;
    if (!save_tu_page(translated_tb_vector, tb_count,
                      &page.head.curr_page_sum_size, &saved_record_count,
                      &saved_tb_count)) {
        goto out;
    }
    page.head.tb_count = saved_record_count;

    page_ok = aot_page_writer_finish(&page);
    if (page_ok) {
        ctx->output_tb_count += saved_tb_count;
    }

out:
    for (int i = 0; translated_tb_vector && i < tb_count; i++) {
        g_free(translated_tb_vector[i]);
    }
    g_free(translated_tb_vector);
    aot_page_writer_cleanup(&page);
    return page_ok;
}

static bool save_tu_cross_page(AOTSaveContext *ctx,
        TranslationBlock **curr_tb_vector, int curr_tb_num)
{
    AOTCrossPageSpool spool = { 0 };
    bool ok = false;

    if (!curr_tb_num) {
        return true;
    }

    aot_prepare_save_cpu();
    clear_tu_rel_table();
    CPUArchState *env = offline_cpu->env_ptr;
    uint32_t mtcc_id = curr_mtcc(env);
    tcg_ctx = tcg_ctx_mtcc[mtcc_id];
    spool.code_rewind = qatomic_read(&tcg_ctx->code_gen_ptr);
    spool.mtcc_id = mtcc_id;
    spool.file = tmpfile();
    if (!spool.file) {
        fprintf(stderr, "[AOT] Save: create cross-page TU spool failed\n");
        goto out;
    }
    spool.copy_buffer = g_try_malloc(AOT_TU_COPY_BUFFER_SIZE);
    if (!spool.copy_buffer) {
        fprintf(stderr, "[AOT] Save: alloc cross-page copy buffer failed\n");
        goto out;
    }

    /* No later consumer requires the collected vector's original order. */
    if (!retranslate_by_tu_cross_page(offline_cpu, curr_tb_vector,
                                      curr_tb_num,
                                      aot_spool_cross_page_tu, &spool)) {
        fprintf(stderr, "[AOT] Save: cross-page TU retranslation failed\n");
        goto out;
    }
    if (spool.mapping_rejects[AOT_TU_MAPPING_PAGE_LIMIT] ||
            spool.mapping_rejects[AOT_TU_MAPPING_CONFLICT] ||
            spool.mapping_rejects[AOT_TU_MAPPING_INVALID_RAM]) {
        aot_verbose_log("[AOT][Save] Rejected TU attempts: page_limit=%d, "
                "mapping_conflict=%d, invalid_ram=%d (includes retries)\n",
                spool.mapping_rejects[AOT_TU_MAPPING_PAGE_LIMIT],
                spool.mapping_rejects[AOT_TU_MAPPING_CONFLICT],
                spool.mapping_rejects[AOT_TU_MAPPING_INVALID_RAM]);
    }
    if (fflush(spool.file) == EOF) {
        fprintf(stderr, "[AOT] Save: cross-page TU spool flush failed\n");
        goto out;
    }
    qsort(spool.refs, spool.ref_count, sizeof(*spool.refs), aot_save_ref_cmp);

    for (size_t l = 0; l < spool.ref_count; ) {
        size_t r = l + 1;
        tb_page_addr_t page_addr = spool.refs[l].ram_page_addr;

        while (r < spool.ref_count &&
                aot_save_refs_same_page(&spool.refs[l], &spool.refs[r])) {
            r++;
        }
        if (r - l > INT_MAX) {
            fprintf(stderr, "[AOT] Save: too many TU records in one page\n");
            goto out;
        }

        AOTPageWriter page;
        if (!aot_page_writer_begin(ctx, &page, page_addr, 0)) {
            goto out;
        }
        for (size_t i = l; i < r; i++) {
            const AOTTUSaveRef *ref = &spool.refs[i];

            if (i > l && aot_save_refs_same_record(&spool.refs[i - 1], ref)) {
                continue;
            }
            if (!aot_copy_spooled_tu(&spool, ref,
                                     &page.head.curr_page_sum_size)) {
                aot_page_writer_cleanup(&page);
                goto out;
            }
            page.head.tb_count++;
            if (ref->anchor_delta == 0) {
                const AOTTUSpoolRecord *record =
                    &spool.records[ref->tu_index];

                if (ctx->output_tb_count > INT_MAX - record->tb_count) {
                    fprintf(stderr,
                            "[AOT] Save: too many cross-page AOT TBs\n");
                    aot_page_writer_cleanup(&page);
                    goto out;
                }
                ctx->output_tb_count += record->tb_count;
            }
        }
        if (!aot_page_writer_finish(&page)) {
            aot_page_writer_cleanup(&page);
            goto out;
        }
        aot_page_writer_cleanup(&page);
        l = r;
    }
    if (fflush(out_tb_file) == EOF) {
        fprintf(stderr, "[AOT] Save: cross-page TU flush failed\n");
        goto out;
    }
    ok = true;

out:
    if (spool.code_rewind) {
        aot_cross_page_reset_translation(&spool);
    }
    if (spool.file) {
        fclose(spool.file);
    }
    g_free(spool.copy_buffer);
    g_free(spool.refs);
    g_free(spool.records);
    return ok;
}

static bool save_tu_by_page(AOTSaveContext *ctx,
        TranslationBlock **curr_tb_vector, int curr_tb_num, uint32_t aot_mode)
{
    if (!curr_tb_num) {
        return true;
    }

    aot_prepare_save_cpu();

    if (!curr_tb_num) {
        return true;
    }

    int l = 0, r = 1;
    uint64_t curr_hva = curr_tb_vector[0]->hva;

    while (r < curr_tb_num) {
        if (curr_tb_vector[r]->hva != curr_hva) {
            if (!deal_tu_page(ctx, curr_tb_vector + l, r - l,
                              aot_mode)) {
                return false;
            }
            l = r;
            curr_hva = curr_tb_vector[r]->hva;
        }

        r++;
    }
    return deal_tu_page(ctx, curr_tb_vector + l, r - l, aot_mode);
}
#endif

static bool aot_save_page_info_tail(AOTSaveContext *ctx, uint32_t aot_mode)
{
    size_t page_info_size;

    if (!aot_page_info_size(ctx->page_info_count, &page_info_size)) {
        fprintf(stderr, "page info array size overflow\n");
        return false;
    }

    page_info_array_t *info_array = malloc(page_info_size);
    if (!info_array) {
        fprintf(stderr, "malloc page_info_array fail\n");
        return false;
    }

    info_array->page_count = ctx->page_info_count;
    memcpy(info_array->pages, ctx->page_info_list,
            ctx->page_info_count * sizeof(page_info_t));
    if (!aot_save_write(info_array, page_info_size, 1)) {
        fprintf(stderr, "save page info array fail\n");
        free(info_array);
        return false;
    }
    if (ctx->output_tb_count > INT_MAX ||
            !save_aot_tail(ctx->page_info_count, ctx->output_tb_count,
                aot_mode)) {
        free(info_array);
        return false;
    }

    free(info_array);
    aot_save_context_free_page_info(ctx);
    return true;
}

static void generate_aot_file(AOTSaveContext *ctx,
        const char *aot_file_prefix,
        uint32_t aot_tb_num, uint32_t aot_mode)
{
    aot_save_context_free_page_info(ctx);

    AOTFileWriter writer;

    aot_file_writer_init(&writer, aot_file_path, aot_file_prefix);
    if (!aot_file_writer_open(&writer)) {
        goto fail;
    }
    aot_verbose_log("[AOT][Save pid=%d] Start: %s (%d TBs)\n", getpid(),
            writer.final_name, aot_tb_num);
    if (option_verbose) {
        fprintf(stderr, "[AOT][Save] Temporary file: %s\n", writer.tmp_name);
    }

    TranslationBlock **curr_tb_vector = (aot_mode & AOT_CORE) ?
        ctx->core_tb_vector : ctx->user_tb_vector;
#ifdef CONFIG_LATX_TU
    if (aot_tu_enabled() && (aot_mode & AOT_USER)) {
        bool saved = option_aot_cross_page_tu ?
            save_tu_cross_page(ctx, curr_tb_vector, aot_tb_num) :
            save_tu_by_page(ctx, curr_tb_vector, aot_tb_num, aot_mode);
        if (!saved) {
            goto fail;
        }
    } else
#endif
    {
        if (!save_tb_by_page(ctx, curr_tb_vector, aot_tb_num, aot_mode)) {
            goto fail;
        }
    }

    /* Writing the tail also releases page info and resets these counters. */
    uint32_t output_pages = ctx->page_info_count;
    uint64_t output_tbs = ctx->output_tb_count;
    if (!aot_save_page_info_tail(ctx, aot_mode)) {
        goto fail;
    }

    if (!aot_file_writer_commit(&writer)) {
        goto fail;
    }
    aot_verbose_log("[AOT][Save pid=%d] Committed: %s, "
            "pages=%u, TBs=%" PRIu64 "\n",
            getpid(), writer.final_name, output_pages, output_tbs);

#if DEBUG_AOT
    ctx->page_num = 0;
    out_tb_file_tb_count = 0;
    ctx->skip_page_num = 0;
#endif

    return;

fail:
    fprintf(stderr, "[AOT] Save: failed to generate %s\n",
            aot_file_prefix);
    aot_file_writer_abort(&writer);
    aot_save_context_free_page_info(ctx);
    aot_save_fatal();
}

/*
 * Runtime save has at most one active child per process.  child_pid owns the
 * in-flight snapshot.  The child reports its completed outputs only after all
 * requested add files have been committed.
 */
static pid_t aot_runtime_save_get_child_pid(void)
{
    AOTRuntimeSaveState *runtime = aot_save_state_runtime();

    return qatomic_read(&runtime->child_pid);
}

static void aot_runtime_save_set_child(pid_t pid)
{
    AOTRuntimeSaveState *runtime = aot_save_state_runtime();

    qatomic_set(&runtime->child_pid, pid);
}

static void aot_runtime_save_clear_child(void)
{
    AOTRuntimeSaveState *runtime = aot_save_state_runtime();

    qatomic_set(&runtime->child_pid, 0);
    if (runtime->completion_fd >= 0) {
        close(runtime->completion_fd);
        runtime->completion_fd = -1;
    }
}

static void aot_runtime_save_lock(void)
{
    AOTRuntimeSaveState *runtime = aot_save_state_runtime();

    pthread_mutex_lock(&runtime->wait_mutex);
}

static void aot_runtime_save_unlock(void)
{
    AOTRuntimeSaveState *runtime = aot_save_state_runtime();

    pthread_mutex_unlock(&runtime->wait_mutex);
}

static void aot_child_finish(pid_t pid, int status)
{
    uint32_t merge_req = 0;

    if (WIFSIGNALED(status)) {
        /* A base may have committed before the child lost its status mask. */
        merge_req = AOT_CORE | AOT_USER;
        fprintf(stderr, "[AOT] child pid=%d killed by signal %d (%s)\n",
                pid, WTERMSIG(status),
                WTERMSIG(status) == SIGKILL ? "SIGKILL" :
                WTERMSIG(status) == SIGSEGV ? "SIGSEGV" :
                WTERMSIG(status) == SIGABRT ? "SIGABRT" : "");
    } else if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);

        if ((code & ~(AOT_CORE | AOT_USER | AOT_CHILD_MERGE_FAILED)) ==
                AOT_CHILD_EXIT_BASE) {
            merge_req = code & (AOT_CORE | AOT_USER);
            if (code & AOT_CHILD_MERGE_FAILED) {
                fprintf(stderr, "[AOT][Save pid=%d] Failed: merge incomplete, "
                        "files_mask=0x%x\n", pid, merge_req);
            } else if (option_verbose) {
                fprintf(stderr,
                        "[AOT][Save pid=%d] Finished: files_mask=0x%x\n",
                        pid, merge_req);
            }
        } else {
            /* Saving a later file can fail after an earlier commit. */
            merge_req = AOT_CORE | AOT_USER;
            fprintf(stderr, "[AOT][Save pid=%d] Failed: exit=%d\n", pid, code);
        }
    }

    if (merge_req) {
        aot_merger_request(merge_req);
    }
}

/* Return true when a runtime save child is still in flight. */
static bool aot_reap_save_child(bool block)
{
    int status;
    pid_t pid;
    pid_t ret;

    aot_runtime_save_lock();
    pid = aot_runtime_save_get_child_pid();
    if (pid <= 0) {
        aot_runtime_save_unlock();
        return false;
    }
    do {
        ret = waitpid(pid, &status, block ? 0 : WNOHANG);
    } while (block && ret < 0 && errno == EINTR);

    if (ret == 0) {
        aot_runtime_save_unlock();
        return true;
    }

    if (ret == pid) {
        aot_child_finish(pid, status);
        aot_runtime_save_clear_child();
        aot_runtime_save_unlock();
        return false;
    }

    if (ret < 0 && errno == EINTR) {
        aot_runtime_save_unlock();
        return true;
    } else {
        fprintf(stderr, "[AOT] waitpid child pid=%d failed: %s\n",
                pid, strerror(errno));
        aot_runtime_save_clear_child();
    }
    aot_runtime_save_unlock();
    return false;
}

static bool aot_poll_save_child(void)
{
    return aot_reap_save_child(false);
}

static void aot_wait_save_child(void)
{
    aot_reap_save_child(true);
}

static void aot_save_capture_runtime_context(void)
{
    if (!offline_cpu) {
        offline_cpu = global_offline_cpu;
    }
    if (offline_cpu) {
        global_offline_cpu = offline_cpu;
    }
    if (lsenv) {
        global_lsenv = lsenv;
    }
    if (tu_data) {
        global_tu_data = tu_data;
    }
}

static void aot_flush_save_trigger_init(AOTFlushSaveTrigger *trigger,
        int mtcc_id)
{
    *trigger = (AOTFlushSaveTrigger) {
        .mtcc_id = mtcc_id,
        .has_core = false,
        .has_user = false,
    };
}

static pid_t aot_start_save_child(AOTFlushSaveJob *job);

static bool aot_flush_save_trigger_prepare(void)
{
    aot_save_capture_runtime_context();
    if (!global_offline_cpu) {
        fprintf(stderr, "[AOT] runtime save has no offline CPU context\n");
        return false;
    }

    return true;
}

static void aot_save_run_sync(int mtcc_id)
{
    AOTSaveContext *ctx = aot_save_state_context();

    fprintf(stderr, "[AOT][Shutdown] Synchronous save started\n");
    aot_save_context_begin_translate();
    aot_tu_merge_cpu = offline_cpu;
    if (!get_tb(ctx, mtcc_id, false)) {
        goto out;
    }
    if (!aot_save_context_has_min_tb(ctx, AOT_SYNC_SAVE_MIN_TB)) {
        goto drain;
    }

    if (ctx->core_tb_num) {
        generate_aot_file(ctx, "sys-aot-core",
                          ctx->core_tb_num, AOT_CORE);
    }
    if (ctx->user_tb_num) {
        generate_aot_file(ctx, "sys-aot-user",
                          ctx->user_tb_num, AOT_USER);
    }

drain:
    aot_merger_drain();

out:
    aot_tu_merge_cpu = NULL;
    aot_save_context_end_translate(ctx);
}

void aot_save_on_shutdown(void)
{
    qatomic_set(&aot_merge_shutting_down, 1);
    /* Reap if ready, but keep a final snapshot even with a save in flight. */
    aot_poll_save_child();
    aot_merger_stop();
    aot_save_capture_runtime_context();
    if (!offline_cpu) {
        offline_cpu = global_offline_cpu;
    }
    if (!lsenv) {
        lsenv = global_lsenv;
    }
    if (!tu_data) {
        tu_data = global_tu_data;
    }
    if (offline_cpu && lsenv && tu_data) {
        AOTFlushSaveTrigger trigger;
        AOTFlushSaveJob job = {
            .ctx = aot_save_state_context(),
            .shutdown = true,
            .previous_save_fd = aot_save_state_runtime()->completion_fd,
        };

        aot_flush_save_trigger_init(&trigger, -1);
        job.trigger = trigger;
        if (aot_flush_save_trigger_prepare()) {
            pid_t pid = aot_start_save_child(&job);
            if (pid > 0) {
                aot_verbose_log("[AOT][Shutdown] Save child pid=%d started\n",
                        pid);
                if (option_wait_aot) {
                    int status;
                    pid_t waited;

                    do {
                        waited = waitpid(pid, &status, 0);
                    } while (waited < 0 && errno == EINTR);
                    if (waited < 0) {
                        perror("[AOT][Shutdown] waitpid");
                    } else {
                        aot_verbose_log("[AOT][Shutdown] Save child pid=%d "
                                "finished, status=0x%x\n", pid, status);
                    }
                }
            } else if (pid < 0) {
                int fork_errno = errno;

                printf("%s:%d\n", __func__, __LINE__);
                printf("%s:%d\n", __func__, __LINE__);
                printf("%s:%d\n", __func__, __LINE__);
                printf("%s:%d\n", __func__, __LINE__);
                perror("fork");
                fprintf(stderr, "[AOT][Shutdown] fork failed: %s; "
                        "falling back to synchronous save\n",
                        strerror(fork_errno));
                aot_wait_save_child();
                aot_save_run_sync(-1);
            }
        }
    }
}

/* Release monitor/display connections, keeping stdout/stderr for save logs. */
static void aot_save_child_detach(AOTFlushSaveJob *job)
{
    struct dirent *entry;
    struct stat st;
    DIR *dir;
    int fd;

    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) {
        aot_save_fatal();
    }
    if (fd > STDERR_FILENO) {
        close(fd);
    }

    /* Translation still needs mapped backends and RCU eventfds. */
    dir = opendir("/proc/self/fd");
    if (!dir) {
        aot_save_fatal();
    }
    while ((entry = readdir(dir))) {
        fd = atoi(entry->d_name);
        if (fd <= STDERR_FILENO || fd == dirfd(dir) ||
            fd == job->completion_write_fd || fd == job->previous_save_fd) {
            continue;
        }
        if (fstat(fd, &st) == 0 &&
            (S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode))) {
            close(fd);
        }
    }
    closedir(dir);
}

static void aot_save_child_main(void *opaque)
{
    AOTFlushSaveJob *job = opaque;
    AOTSaveContext *ctx = job->ctx;
    uint32_t generated = 0;
    int merge_status = 0;
    sigset_t mask;

    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    if (job->completion_read_fd >= 0) {
        close(job->completion_read_fd);
    }
    aot_save_child_detach(job);
    aot_verbose_log("[AOT][Save pid=%d] Started%s\n", getpid(),
            job->shutdown ? " (final snapshot)" : "");
    if (job->previous_save_fd >= 0) {
        char unused;
        ssize_t ret;

        /* The previous worker is a sibling: wait for EOF, not waitpid(). */
        do {
            ret = read(job->previous_save_fd, &unused, 1);
        } while (ret > 0 || (ret < 0 && errno == EINTR));
        close(job->previous_save_fd);
        if (ret < 0) {
            aot_save_fatal();
        }
    }
    /*
     * All O(N) snapshot work happens here.  Runtime TB state cleanup remains
     * in the parent, while this process only reads its fork snapshot.
     */
    aot_runtime_save_child = true;
    offline_cpu = global_offline_cpu;
    if (!lsenv) lsenv = global_lsenv;
    if (!tu_data) tu_data = global_tu_data;
    if (tu_data) tu_trees_reset();
    aot_save_context_begin_translate();
    aot_tu_merge_cpu = offline_cpu;

    if (job->merge_only) {
        goto merge;
    }
    if (!get_tb(ctx, job->trigger.mtcc_id, true)) {
        aot_save_fatal();
    }
    if (!aot_save_context_select_flush_outputs(ctx,
                &job->trigger.has_core, &job->trigger.has_user)) {
        goto merge;
    }

    if (job->trigger.has_core) {
        generate_aot_file(ctx, "sys-aot-core",
                          ctx->core_tb_num, AOT_CORE);
        generated |= AOT_CORE;
    }
    if (job->trigger.has_user) {
        generate_aot_file(ctx, "sys-aot-user",
                          ctx->user_tb_num, AOT_USER);
        generated |= AOT_USER;
    }
merge:
    if (job->shutdown) {
        int merged = aot_merge_all("sys-aot-core", AOT_CORE);

        if (merged < 0) {
            merge_status = AOT_CHILD_MERGE_FAILED;
        }
    }
    if (job->shutdown || aot_cross_page_tu_enabled()) {
        int merged = aot_merge_all("sys-aot-user", AOT_USER);

        if (merged != 0) {
            /* A failed drain may already have committed earlier increments. */
            generated |= AOT_USER;
        }
        if (merged < 0) {
            merge_status = AOT_CHILD_MERGE_FAILED;
        }
    }
    aot_save_context_free_tb(ctx);
    aot_verbose_log("[AOT][Save pid=%d] Finished: files_mask=0x%x, "
            "merge_status=%d\n", getpid(), generated, merge_status);
    _exit(AOT_CHILD_EXIT_BASE | generated | merge_status);
}

static pid_t aot_start_save_child(AOTFlushSaveJob *job)
{
    pid_t pid;
    int completion[2] = { -1, -1 };
#ifdef CONFIG_SOFTMMU
    CPUState *cpu = offline_cpu ? offline_cpu : global_offline_cpu;
    CPUArchState *env = cpu ? cpu->env_ptr : NULL;
#endif

    if (!job->shutdown) {
        if (pipe(completion) < 0) {
            return -1;
        }
        job->previous_save_fd = -1;
    }
    job->completion_read_fd = completion[0];
    job->completion_write_fd = completion[1];
    /*
     * A thread executing in DUNE must fork through its host loop.  The
     * shutdown main thread is already in host context and uses normal fork,
     * even for a HAMT guest.  hamt_is_running() is thread-local.
     */
#ifdef CONFIG_SOFTMMU
    if (hamt_enabled() && hamt_is_running() &&
            env && env->hamt_dune_kvm_cpu) {
        pid = hamt_fake_fork(env->hamt_dune_kvm_cpu, aot_save_child_main, job);
    } else
#endif
    {
        /* The ordinary host path has the same atfork requirements as HAMT. */
        bool took_bql = !qemu_mutex_iothread_locked();

        assert(get_ptr_rcu_reader()->depth == 0);
        if (took_bql) {
            qemu_mutex_lock_iothread();
        }
        pid = rcu_fork_snapshot();
        int saved_errno = errno;

        if (took_bql) {
            qemu_mutex_unlock_iothread();
        }
        errno = saved_errno;
        if (pid == 0) {
            aot_save_child_main(job);
        }
    }
    if (!job->shutdown) {
        int saved_errno = errno;

        close(completion[1]);
        if (pid > 0) {
            aot_save_state_runtime()->completion_fd = completion[0];
        } else {
            close(completion[0]);
        }
        errno = saved_errno;
    }
    return pid;
}

void aot_save_on_tb_flush(int mtcc_id)
{
    AOTSaveContext *ctx = aot_save_state_context();
    AOTFlushSaveTrigger trigger;

    if (in_aot_translate) {
        return;
    }
    if (!option_sys_aot || !option_aot_save) {
        return;
    }
    if (!option_aot_fork) {
        return;
    }

    /* tb_flush_mtcc() supplies either a serial or an exclusive context. */
    g_assert(cpu_in_serial_context(current_cpu));

    /* Reap a completed runtime save; skip this flush if a save is running. */
    if (aot_poll_save_child()) {
        return;
    }
    /* if(mtcc_id!=0)return; */

    aot_flush_save_trigger_init(&trigger, mtcc_id);
    if (!aot_flush_save_trigger_prepare()) {
        return;
    }

    aot_save_context_begin_translate();

    waitpid(-1, NULL, WNOHANG);

    AOTFlushSaveJob child_job = {
        .ctx = ctx,
        .trigger = trigger,
    };
    pid_t pid = aot_start_save_child(&child_job);

    if (pid > 0) {
        aot_runtime_save_set_child(pid);
    }

    if (pid < 0) {
        perror("fork");
    }

    aot_save_context_end_translate(ctx);
}

/* Requested by the merger; the callback executes with all vCPUs quiescent. */
static int aot_merge_work_queued;

static void aot_run_merge_work(CPUState *cpu, run_on_cpu_data request)
{
    AOTFlushSaveJob job = { .ctx = aot_save_state_context(),
                           .merge_only = true };
    pid_t pid;

    qatomic_set(&aot_merge_work_queued, 0);
    if (qatomic_read(&aot_merge_shutting_down) ||
        aot_poll_save_child() || !request.host_int) {
        return;
    }
    offline_cpu = cpu;
    if (!aot_flush_save_trigger_prepare()) {
        return;
    }
    pid = aot_start_save_child(&job);
    if (pid > 0) {
        aot_runtime_save_set_child(pid);
    }
}

bool aot_tu_merge_running(void)
{
    return aot_runtime_save_get_child_pid() > 0;
}

static void aot_schedule_merge_work(void *opaque)
{
    if (!qatomic_read(&aot_merge_shutting_down) &&
        first_cpu && first_cpu->created) {
        async_safe_run_on_cpu(first_cpu, aot_run_merge_work,
            RUN_ON_CPU_HOST_INT(GPOINTER_TO_INT(opaque)));
    } else {
        qatomic_set(&aot_merge_work_queued, 0);
    }
}

void aot_request_tu_merge(bool merge)
{
#ifdef CONFIG_SOFTMMU
    /* Do not wait for BQL: shutdown may join the merger while holding it. */
    if (!qatomic_read(&aot_merge_shutting_down) &&
        !qatomic_xchg(&aot_merge_work_queued, 1)) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), aot_schedule_merge_work,
                                GINT_TO_POINTER(merge));
    }
#else
    (void)merge;
    (void)aot_schedule_merge_work;
#endif
}
