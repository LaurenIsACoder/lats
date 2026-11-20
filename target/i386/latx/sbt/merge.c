/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sys-aot.h"
#include "sys-aot-internal.h"
#include "sys-aot-merge.h"
#include "sys-aot-format.h"
#include "sys-aot-tu-merge.h"
#include "sys-aot-common.h"
#include "file_ctx.h"
#include "latx-options.h"
#include <dirent.h>
#include <limits.h>
#include <sys/mman.h>

static int find_first_add_file(const char *file_name)
{
    char prefix[PATH_MAX];
    DIR *dir;
    struct dirent *entry;
    int first = 0;

    if (snprintf(prefix, sizeof(prefix), "%s-add", file_name) >=
            (int)sizeof(prefix)) {
        return 0;
    }
    dir = opendir(aot_file_path);
    if (!dir) {
        return 0;
    }
    while ((entry = readdir(dir)) != NULL) {
        char *end;
        long index;

        if (!g_str_has_prefix(entry->d_name, prefix)) {
            continue;
        }
        errno = 0;
        index = strtol(entry->d_name + strlen(prefix), &end, 10);
        if (errno || *end != '\0' || index <= 0 || index > INT_MAX) {
            continue;
        }
        if (first == 0 || index < first) {
            first = index;
        }
    }
    closedir(dir);
    return first;
}

uintptr_t find_other_file_page(uintptr_t file_ptr, uint64_t file_size, uint32_t h, void *guest_code)
{
    aot_tail *tail = (aot_tail *)(file_ptr + file_size - sizeof(aot_tail));
    page_info_array_t *info_array = (page_info_array_t *)((uint8_t *)tail -
        (sizeof(page_info_array_t) + tail->page_num * sizeof(page_info_t)));

    for (uint32_t i = 0; i < info_array->page_count; i++) {
        if (info_array->pages[i].page_hash == h) {
            uint64_t page_addr = file_ptr + info_array->pages[i].file_offset;
            page_head *ph = (page_head *)page_addr;
            if (ph->page_magic == AOT_PAGE_MAGIC && ph->page_hash == h &&
                memcmp(guest_code, (void*)(page_addr + sizeof(page_head)), TARGET_PAGE_SIZE) == 0) {
                return page_addr;
            }
        }
    }
    return 0;
}

typedef struct AOTMergeContext {
    int merged_tb_count;
    uint64_t reduction_tb_count;
    bool write_err;
    int write_errno;
    page_info_t *page_info_list;
    uint32_t page_info_capacity;
    uint32_t page_info_count;
    FILE *out_file;
    void *input[2];
    uint64_t input_size[2];
} AOTMergeContext;

static bool aot_merge_write(AOTMergeContext *ctx, const void *data, size_t size)
{
    errno = 0;
    if (fwrite(data, size, 1, ctx->out_file) != 1) {
        if (!ctx->write_errno) {
            ctx->write_errno = errno ? errno : EIO;
        }
        ctx->write_err = true;
        return false;
    }
    return true;
}

static inline bool save_tb(AOTMergeContext *ctx, uintptr_t tb_table,
        aot_block *tb, int *pre_offset,
        uint32 *pre_flags, uint32_t *pre_cflags)
{
    int offset = (tb->pc & ~TARGET_PAGE_MASK);
    if (*pre_offset == offset
            && *pre_flags == tb->flags
            && *pre_cflags == tb->cflags
            ) {
        ctx->reduction_tb_count++;
        return false;
    }
    if (offset < *pre_offset) {
        fprintf(stderr, "[AOT] Merge: TB offset order error: pre=%x curr=%x\n",
                *pre_offset, offset);
        ctx->write_err = true;
        return false;
    }
    assert(*pre_offset <= offset);
    uint32_t tb_sum_size = *(uint32_t *)(tb_table + 4);
    if (!aot_merge_write(ctx, (void *)tb_table, tb_sum_size)) {
        ctx->write_err = true;
        return false;
    }
    *pre_offset = offset;
    *pre_flags = tb->flags;
    *pre_cflags = tb->cflags;
    ctx->merged_tb_count++;
    return true;
}

static bool aot_tb_cmp(aot_block *tb1, aot_block *tb2)
{
    int offset1 = tb1->pc & ~TARGET_PAGE_MASK;
    int offset2 = tb2->pc & ~TARGET_PAGE_MASK;
    if (offset1 < offset2) {
        return true;
    } else if (offset1 == offset2) {
        if (tb1->cflags < tb2->cflags) {
            return true;
        } else if (tb1->cflags == tb2->cflags) {
            if (tb1->flags < tb2->flags) {
                return true;
            }
        }
    }
    return false;
}

static void aot_merge_context_free(AOTMergeContext *ctx)
{
    /* These private merge mappings are not published to runtime readers. */
    for (int i = 0; i < 2; i++) {
        if (ctx->input[i]) {
            munmap(ctx->input[i], ctx->input_size[i]);
            ctx->input[i] = NULL;
            ctx->input_size[i] = 0;
        }
    }
    free(ctx->page_info_list);
    ctx->page_info_list = NULL;
    ctx->page_info_capacity = 0;
    ctx->page_info_count = 0;
}

static void aot_merge_close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void aot_merge_unlock_fd(int *fd)
{
    if (*fd >= 0) {
        flock_set(*fd, F_UNLCK, true);
        aot_merge_close_fd(fd);
    }
}

static bool aot_merge_context_reserve_page_info(AOTMergeContext *ctx)
{
    if (ctx->page_info_count < ctx->page_info_capacity) {
        return true;
    }

    uint32_t new_capacity = ctx->page_info_capacity ?
        ctx->page_info_capacity * 2 : 1024;
    page_info_t *new_page_info = realloc(ctx->page_info_list,
            new_capacity * sizeof(page_info_t));
    if (!new_page_info) {
        fprintf(stderr, "[AOT] Merge: realloc page_info_list failed\n");
        ctx->write_err = true;
        return false;
    }

    ctx->page_info_list = new_page_info;
    ctx->page_info_capacity = new_capacity;
    return true;
}

static bool aot_merge_context_append_page_info(AOTMergeContext *ctx,
        uint32_t page_hash, uint64_t file_offset)
{
    if (!aot_merge_context_reserve_page_info(ctx)) {
        return false;
    }
    ctx->page_info_list[ctx->page_info_count].page_hash = page_hash;
    ctx->page_info_list[ctx->page_info_count].file_offset = file_offset;
    ctx->page_info_count++;
    return true;
}

static void save_one_page(AOTMergeContext *ctx, uintptr_t page)
{
    page_head *ph = (page_head *)page;
    uint64_t offset_in_file = ftell(ctx->out_file);

    if (!aot_merge_write(ctx, (void *)page, ph->curr_page_sum_size)) {
        ctx->write_err = true;
        return;
    }

    if (!aot_merge_context_append_page_info(ctx, ph->page_hash,
            offset_in_file)) {
        return;
    }
}

static const aot_tu_page *aot_merge_tu_pages(const aot_tu_head *tu)
{
    return (const aot_tu_page *)(tu + 1);
}

static const aot_block *aot_merge_tu_blocks(const aot_tu_head *tu)
{
    return (const aot_block *)(aot_merge_tu_pages(tu) + tu->page_count);
}

/*
 * Same runtime lookup keys and dependencies means the add-file TU replaces
 * the base-file TU.  Absolute saved PCs are deliberately ignored so ASLR does
 * not retain duplicate records for the same page-relative translation.
 */
static bool aot_same_tu_identity(const aot_tu_head *a,
                                 const aot_tu_head *b)
{
    const aot_tu_page *ap = aot_merge_tu_pages(a);
    const aot_tu_page *bp = aot_merge_tu_pages(b);
    const aot_block *ab = aot_merge_tu_blocks(a);
    const aot_block *bb = aot_merge_tu_blocks(b);
    vaddr abase = a->first_pc & TARGET_PAGE_MASK;
    vaddr bbase = b->first_pc & TARGET_PAGE_MASK;

    if (a->tu_id != b->tu_id ||
            (a->first_pc & ~TARGET_PAGE_MASK) !=
                (b->first_pc & ~TARGET_PAGE_MASK) ||
            a->tb_count != b->tb_count || a->page_count != b->page_count ||
            memcmp(ap, bp, a->page_count * sizeof(*ap)) != 0) {
        return false;
    }
    for (uint32_t i = 0; i < a->tb_count; i++) {
        if (ab[i].pc - abase != bb[i].pc - bbase ||
                ab[i].cs_base != bb[i].cs_base ||
                ab[i].flags != bb[i].flags ||
                ab[i].cflags != bb[i].cflags) {
            return false;
        }
    }
    return true;
}

typedef struct AOTWrittenTURecord {
    const void *record;
    uint32_t magic;
} AOTWrittenTURecord;

static bool aot_same_tu_ref(const aot_tu_ref *a, const aot_tu_ref *b)
{
    return a->tu_id == b->tu_id && a->anchor_delta == b->anchor_delta;
}

static bool aot_write_unique_tu_record(AOTMergeContext *ctx,
        const void *record, AOTWrittenTURecord *written,
        uint32_t *written_count, page_head *new_head)
{
    uint32_t magic = *(const uint32_t *)record;
    uint32_t record_size;
    uint32_t record_tb_count = 0;

    if (magic == AOT_TB_MAGIC) {
        const aot_tu_head *tu = record;

        record_size = tu->tu_sum_size;
        record_tb_count = tu->tb_count;
    } else if (magic == AOT_TU_REF_MAGIC) {
        record_size = sizeof(aot_tu_ref);
    } else {
        ctx->write_err = true;
        return false;
    }

    for (uint32_t i = 0; i < *written_count; i++) {
        bool same = false;

        if (written[i].magic == magic) {
            same = magic == AOT_TB_MAGIC ?
                aot_same_tu_identity(record, written[i].record) :
                aot_same_tu_ref(record, written[i].record);
        }
        if (same) {
            ctx->reduction_tb_count += record_tb_count;
            return true;
        }
    }
    if (record_size > INT32_MAX - new_head->curr_page_sum_size ||
            !aot_merge_write(ctx, record, record_size)) {
        ctx->write_err = true;
        return false;
    }
    written[*written_count] = (AOTWrittenTURecord) {
        .record = record,
        .magic = magic,
    };
    (*written_count)++;
    new_head->tb_count++;
    new_head->curr_page_sum_size += record_size;
    ctx->merged_tb_count += record_tb_count;
    return true;
}

static bool aot_merge_tu_records(AOTMergeContext *ctx, uintptr_t page,
        AOTWrittenTURecord *written, uint32_t *written_count,
        page_head *new_head)
{
    const page_head *ph = (const page_head *)page;
    uintptr_t record = page + sizeof(*ph) + TARGET_PAGE_SIZE;
    uint32_t parsed_records = 0;

    while (parsed_records < ph->tb_count) {
        uint32_t magic = *(uint32_t *)record;
        uint32_t record_size = magic == AOT_TB_MAGIC ?
            ((const aot_tu_head *)record)->tu_sum_size :
            magic == AOT_TU_REF_MAGIC ? sizeof(aot_tu_ref) : 0;

        if (!record_size || !aot_write_unique_tu_record(ctx,
                (const void *)record, written, written_count, new_head)) {
            return false;
        }
        parsed_records++;
        record += record_size;
    }
    return true;
}

static bool aot_count_page_tu_tbs(uintptr_t page, int *tb_count)
{
    const page_head *ph = (const page_head *)page;
    uintptr_t record = page + sizeof(*ph) + TARGET_PAGE_SIZE;
    uint64_t total = 0;

    for (uint32_t i = 0; i < ph->tb_count; i++) {
        uint32_t magic = *(uint32_t *)record;

        if (magic == AOT_TB_MAGIC) {
            const aot_tu_head *tu = (const aot_tu_head *)record;

            total += tu->tb_count;
            record += tu->tu_sum_size;
        } else if (magic == AOT_TU_REF_MAGIC) {
            record += sizeof(aot_tu_ref);
        } else {
            return false;
        }
        if (total > INT_MAX) {
            return false;
        }
    }
    *tb_count = total;
    return true;
}

static void save_same_tu_page(AOTMergeContext *ctx, uintptr_t page1,
                              uintptr_t page2)
{
    const page_head *ph1 = (const page_head *)page1;
    const page_head *ph2 = (const page_head *)page2;
    uint64_t offset_in_file = ftell(ctx->out_file);
    uint64_t max_records = MAX((uint64_t)ph1->tb_count + ph2->tb_count,
                               UINT64_C(1));
    AOTWrittenTURecord *written = g_try_new(AOTWrittenTURecord, max_records);
    uint32_t written_count = 0;
    page_head new_head = {
        .page_magic = AOT_PAGE_MAGIC,
        .page_hash = ph1->page_hash,
        .tb_count = 0,
        .curr_page_sum_size = sizeof(page_head) + TARGET_PAGE_SIZE,
    };

    if (!written) {
        ctx->write_err = true;
        return;
    }
    if (!aot_merge_write(ctx, &new_head, sizeof(new_head)) ||
            !aot_merge_write(ctx, (const void *)(page1 + sizeof(page_head)),
                             TARGET_PAGE_SIZE) ||
            !aot_merge_tu_records(ctx, page1, written, &written_count,
                                  &new_head) ||
            !aot_merge_tu_records(ctx, page2, written, &written_count,
                                  &new_head)) {
        ctx->write_err = true;
        g_free(written);
        return;
    }
    if (fseek(ctx->out_file, offset_in_file, SEEK_SET) != 0 ||
            !aot_merge_write(ctx, &new_head, sizeof(new_head)) ||
            fseek(ctx->out_file, 0, SEEK_END) != 0 ||
            !aot_merge_context_append_page_info(ctx, new_head.page_hash,
                                                offset_in_file)) {
        ctx->write_err = true;
    }
    g_free(written);
}

static void save_same_page(AOTMergeContext *ctx, uintptr_t page1, uintptr_t page2)
{
    page_head *ph1 = (page_head *)page1;
    page_head *ph2 = (page_head *)page2;
    uint32_t h = ph1->page_hash;
    uint64_t offset_in_file = ftell(ctx->out_file);

    page_head ph_new;
    ph_new.page_magic = AOT_PAGE_MAGIC;
    ph_new.page_hash = h;
    ph_new.tb_count = 0;
    ph_new.curr_page_sum_size= TARGET_PAGE_SIZE + sizeof(page_head);

    if (!aot_merge_write(ctx, &ph_new, sizeof(page_head))) {
        ctx->write_err = true;
        return;
    }
    if (!aot_merge_write(ctx, (void *)(page1 + sizeof(page_head)),
                         TARGET_PAGE_SIZE)) {
        ctx->write_err = true;
        return;
    }

    uint32_t tb_count1 = ph1->tb_count;
    uint32_t tb_count2 = ph2->tb_count;
    uintptr_t tb_table1 = page1 + sizeof(page_head) + TARGET_PAGE_SIZE;
    uintptr_t tb_table2 = page2 + sizeof(page_head) + TARGET_PAGE_SIZE;
    int id1 = 0, id2 = 0;
    int pre_offset = -1;
    uint32_t pre_flags = 0, pre_cflags = 0;

    while (id1 < tb_count1 && id2 < tb_count2) {
        aot_block *tb1 = (aot_block *)(tb_table1 + 16);
        aot_block *tb2 = (aot_block *)(tb_table2 + 16);
        if (aot_tb_cmp(tb1, tb2)) {
            uint32_t sum_size = *(uint32_t *)(tb_table1 + 4);
            if (save_tb(ctx, tb_table1, tb1, &pre_offset, &pre_flags, &pre_cflags)) {
                ph_new.tb_count++;
                ph_new.curr_page_sum_size += sum_size;
            }
            if (ctx->write_err) {
                return;
            }
            tb_table1 += sum_size;
            id1++;
        } else {
            uint32_t sum_size = *(uint32_t *)(tb_table2 + 4);
            if (save_tb(ctx, tb_table2, tb2, &pre_offset, &pre_flags, &pre_cflags)) {
                ph_new.tb_count++;
                ph_new.curr_page_sum_size += sum_size;
            }
            if (ctx->write_err) {
                return;
            }
            tb_table2 += sum_size;
            id2++;
        }
    }
    while (id1 < tb_count1) {
        uint32_t sum_size = *(uint32_t *)(tb_table1 + 4);
        aot_block *tb1 = (aot_block *)(tb_table1 + 16);
        if (save_tb(ctx, tb_table1, tb1, &pre_offset, &pre_flags, &pre_cflags)) {
            ph_new.tb_count++;
            ph_new.curr_page_sum_size += sum_size;
        }
        if (ctx->write_err) {
            return;
        }
        tb_table1 += sum_size;
        id1++;
    }
    while (id2 < tb_count2) {
        uint32_t sum_size = *(uint32_t *)(tb_table2 + 4);
        aot_block *tb2 = (aot_block *)(tb_table2 + 16);
        if (save_tb(ctx, tb_table2, tb2, &pre_offset, &pre_flags, &pre_cflags)) {
            ph_new.tb_count++;
            ph_new.curr_page_sum_size += sum_size;
        }
        if (ctx->write_err) {
            return;
        }
        tb_table2 += sum_size;
        id2++;
    }

    fseek(ctx->out_file, offset_in_file, SEEK_SET);
    if (!aot_merge_write(ctx, &ph_new, sizeof(page_head))) {
        ctx->write_err = true;
        return;
    }
    fseek(ctx->out_file, 0, SEEK_END);

    if (!aot_merge_context_append_page_info(ctx, h, offset_in_file)) {
        return;
    }
}

static int aot_merge_file(const char *file_name, uint32_t aot_mode)
{
    AOTMergeContext ctx = { 0 };
    int result = -2;

    char *lock1 = NULL, *lock2 = NULL;
    char *file_name1 = NULL, *file_name2 = NULL, *merged_file_name = NULL;
    bool cross_tu = aot_cross_page_tu_enabled() && (aot_mode & AOT_USER);
    bool remove_add_file = !cross_tu;
    int name_len = strlen(aot_file_path) + strlen(file_name) + 20;
    file_name1 = malloc(name_len);
    file_name2 = malloc(name_len);
    merged_file_name = malloc(name_len);
    lock1 = malloc(name_len);
    lock2 = malloc(name_len);
    if (!(lock1 && lock2 && file_name1 && file_name2 && merged_file_name)) {
        fprintf(stderr, "[AOT] Merge: malloc file names failed\n");
        goto exit1;
    }

    strcpy(file_name1, aot_file_path);
    strcat(file_name1, file_name);
    strcpy(merged_file_name, file_name1);
    strcat(merged_file_name, "-tmp");
    strcpy(lock1, file_name1);
    strcat(lock1, ".lock");

    int add_idx = find_first_add_file(file_name);
    if (add_idx == 0) {
        result = 0;
        goto exit1;
    }
    if (cross_tu && !aot_tu_merge_cpu) {
        result = -3;
        goto exit1;
    }
    snprintf(file_name2, name_len, "%s-add%d", file_name1, add_idx);
    snprintf(lock2, name_len, "%s-add%d.lock", file_name1, add_idx);

    aot_verbose_log("[AOT][Merge] Start: %s-add%d -> %s\n",
            file_name, add_idx, file_name);

    int lockfd1 = -1, lockfd2 = -1;
    if (file_lock(lock1, &lockfd1, F_WRLCK, false) < 0) {
        aot_merge_close_fd(&lockfd1);
        fprintf(stderr, "[AOT] lock1 fail: %s\n", lock1);
        goto exit_locked;
    }
    if (file_lock(lock2, &lockfd2, F_WRLCK, false) < 0) {
        aot_merge_unlock_fd(&lockfd1);
        aot_merge_close_fd(&lockfd2);
        fprintf(stderr, "[AOT] lock2 fail: %s\n", lock2);
        goto exit_locked;
    }

    ctx.input[0] = load_file_to_mem(file_name2, &ctx.input_size[0], aot_mode);
    ctx.input[1] = load_file_to_mem(file_name1, &ctx.input_size[1], aot_mode);
    uint64_t file1_size = ctx.input_size[0], file2_size = ctx.input_size[1];
    uintptr_t file1_ptr = (uintptr_t)ctx.input[0];
    uintptr_t file2_ptr = (uintptr_t)ctx.input[1];
    if (!file1_ptr || !file2_ptr) {
        goto exit;
    }

    aot_tail *tail2 = (aot_tail *)(file2_ptr + file2_size - sizeof(aot_tail));
    page_info_array_t *info_array2 = (page_info_array_t *)((uint8_t *)tail2 -
        (sizeof(page_info_array_t) + tail2->page_num * sizeof(page_info_t)));
    if (info_array2->page_count != tail2->page_num) {
        fprintf(stderr, "[AOT] Merge: page count mismatch in %s\n", file_name1);
        goto exit;
    }

    ctx.out_file = fopen(merged_file_name, "wb");
    if (!ctx.out_file) {
        fprintf(stderr, "[AOT] open merge output failed: %s\n", merged_file_name);
        goto exit;
    }
    out_tb_file = ctx.out_file;

    if (cross_tu) {
        int output_pages = 0;

        if (!aot_merge_tu_files(ctx.input[0], ctx.input_size[0],
                ctx.input[1], ctx.input_size[1], ctx.out_file, aot_mode,
                &output_pages, &ctx.merged_tb_count)) {
            goto exit;
        }
        ctx.page_info_count = output_pages;
        const aot_tail *add_tail = (const aot_tail *)(file1_ptr + file1_size -
                                                     sizeof(aot_tail));

        ctx.reduction_tb_count = (uint64_t)add_tail->tb_num + tail2->tb_num -
                                 ctx.merged_tb_count;
        goto persist;
    }

    /* Get tail1 to calculate correct file_end for file1 (add file) */
    aot_tail *tail1 = (aot_tail *)(file1_ptr + file1_size - sizeof(aot_tail));
    page_info_array_t *info_array1 = (page_info_array_t *)((uint8_t *)tail1 -
        (sizeof(page_info_array_t) + tail1->page_num * sizeof(page_info_t)));
    uint32_t h;
    uintptr_t file_end;
    uint32_t magic, curr_page_sum_size;
    void *guest_code;
    uintptr_t page1, page2;

    page1 = file1_ptr;
    file_end = (uintptr_t)info_array1;
    int max_file_size = 4096;

    while (page1 < file_end) {
        page_head *ph = (page_head *)page1;
        magic = ph->page_magic;
        if (magic != AOT_PAGE_MAGIC && magic != AOT_MERGED_MAGIC) {
            fprintf(stderr, "[AOT] Merge: invalid source page magic %x\n", magic);
            goto exit;
        }
        h = ph->page_hash;
        curr_page_sum_size = ph->curr_page_sum_size;
        if (magic == AOT_MERGED_MAGIC) {
           page1 += curr_page_sum_size;
           continue;
        }

        guest_code = (void *)(page1 + sizeof(page_head));
        page2 = find_other_file_page(file2_ptr, file2_size, h, guest_code);

        if (page2) {
            if (aot_tu_enabled() && (aot_mode & AOT_USER)) {
                save_same_tu_page(&ctx, page1, page2);
            } else {
                save_same_page(&ctx, page1, page2);
            }
            if (ctx.write_err) {
                remove_add_file = false;
                goto exit;
            }
            *(uint32_t *)page2 = AOT_MERGED_MAGIC;
        } else {
            int tb_count = ph->tb_count;
            if (aot_tu_enabled() && (aot_mode & AOT_USER) &&
                    !aot_count_page_tu_tbs(page1, &tb_count)) {
                goto exit;
            }
            ctx.merged_tb_count += tb_count;
            save_one_page(&ctx, page1);
            if (ctx.write_err) {
                remove_add_file = false;
                goto exit;
            }
        }
        page1 += curr_page_sum_size;
        /*
         * A cross-page TU is a closed set consisting of one canonical
         * payload and references in every other member page.  Stopping the
         * merge at an arbitrary page can retain only one side of that set.
         * The on-disk offsets are 64-bit, so preserve the complete set and
         * keep the legacy size cap only for formats without page references.
         */
        if (!aot_cross_page_tu_enabled() &&
                ftell(ctx.out_file) / (1024 * 1024) > max_file_size) {
            break;
        }
    }

    page2 = file2_ptr;
    file_end = (uintptr_t)info_array2;

    while(page2 < file_end) {
        page_head *ph = (page_head *)page2;
        magic = ph->page_magic;
        if (magic != AOT_PAGE_MAGIC && magic != AOT_MERGED_MAGIC) {
            fprintf(stderr, "[AOT] Merge: invalid base page magic %x\n", magic);
            goto exit;
        }
        curr_page_sum_size = ph->curr_page_sum_size;
        if (magic == AOT_MERGED_MAGIC) {
            page2 += curr_page_sum_size;
            continue;
        }
        int tb_count = ph->tb_count;
        if (aot_tu_enabled() && (aot_mode & AOT_USER) &&
                !aot_count_page_tu_tbs(page2, &tb_count)) {
            goto exit;
        }
        ctx.merged_tb_count += tb_count;
        save_one_page(&ctx, page2);
        if (ctx.write_err) {
            remove_add_file = false;
            goto exit;
        }
        page2 += curr_page_sum_size;
        if (!aot_cross_page_tu_enabled() &&
                ftell(ctx.out_file) / (1024 * 1024) > max_file_size) {
            break;
        }
    }

    if (ctx.write_err) {
        fprintf(stderr, "[AOT] merge write error, discarding output\n");
        remove_add_file = false;
        goto exit;
    }

    size_t page_info_size;
    if (!aot_page_info_size(ctx.page_info_count, &page_info_size)) {
        fprintf(stderr, "[AOT] Merge: page info array size overflow\n");
        remove_add_file = false;
        goto exit;
    }
    page_info_array_t *info_array = malloc(page_info_size);
    if (!info_array) {
        fprintf(stderr, "[AOT] Merge: malloc page_info_array failed\n");
        remove_add_file = false;
        goto exit;
    }
    info_array->page_count = ctx.page_info_count;
    memcpy(info_array->pages, ctx.page_info_list,
            ctx.page_info_count * sizeof(page_info_t));
    if (!aot_merge_write(&ctx, info_array, page_info_size)) {
        fprintf(stderr, "[AOT] Merge: save page info array failed\n");
        free(info_array);
        remove_add_file = false;
        goto exit;
    }
    free(info_array);

    if (!save_aot_tail(ctx.page_info_count, ctx.merged_tb_count, aot_mode)) {
        remove_add_file = false;
        goto exit;
    }
persist:;
    struct stat st = { 0 };
    if (fflush(ctx.out_file) == EOF || fsync(fileno(ctx.out_file)) < 0 ||
            fstat(fileno(ctx.out_file), &st) < 0 ||
            fclose(ctx.out_file) == EOF) {
        fprintf(stderr, "[AOT] Merge: failed to persist output: %s\n",
                strerror(errno));
        ctx.out_file = NULL;
        out_tb_file = NULL;
        remove_add_file = false;
        goto exit;
    }
    ctx.out_file = NULL;
    out_tb_file = NULL;

    if (cross_tu) {
        int fd = open(merged_file_name, O_RDONLY);
        void *mapped = fd < 0 ? MAP_FAILED :
            mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        bool valid = mapped != MAP_FAILED && test_integrity(fd, aot_mode) &&
            validate_aot_layout(mapped, st.st_size);

        if (mapped != MAP_FAILED) {
            munmap(mapped, st.st_size);
        }

        if (fd >= 0) {
            close(fd);
        }
        if (!valid) {
            fprintf(stderr, "[AOT][Merge] Invalid reconstructed TU file\n");
            goto exit;
        }
    }

    aot_verbose_log("[AOT][Merge] Done: %d pages, %d TBs "
            "(deduplicated: %" PRIu64 "), size=%ld MB\n",
            ctx.page_info_count, ctx.merged_tb_count, ctx.reduction_tb_count, st.st_size/1000000);
    if (rename(merged_file_name, file_name1) < 0) {
        fprintf(stderr, "[AOT] Merge: rename %s -> %s failed: %s\n",
                merged_file_name, file_name1, strerror(errno));
        remove_add_file = false;
        goto exit;
    }
    if (remove(file_name2) < 0 && errno != ENOENT) {
        fprintf(stderr, "[AOT] Merge: remove %s failed: %s\n",
                file_name2, strerror(errno));
    }

    aot_merge_unlock_fd(&lockfd1);
    aot_merge_unlock_fd(&lockfd2);
    free(file_name1);
    free(file_name2);
    free(merged_file_name);
    free(lock1);
    free(lock2);
    aot_merge_context_free(&ctx);
    return 1;

exit:
    if (ctx.write_errno) {
        fprintf(stderr, "[AOT] Merge: write %s failed: %s (errno=%d)\n",
                merged_file_name, strerror(ctx.write_errno), ctx.write_errno);
    }
    if (ctx.out_file) {
        fclose(ctx.out_file);
        ctx.out_file = NULL;
        out_tb_file = NULL;
    }
    aot_merge_unlock_fd(&lockfd1);
    aot_merge_unlock_fd(&lockfd2);
    if (merged_file_name) {
        remove(merged_file_name);
    }
    if (remove_add_file && file_name2) {
        remove(file_name2);
    }
exit1:
    if (result < 0 && result != -3) {
        fprintf(stderr, "[AOT] Merge: failed to merge %s\n", file_name);
    }
    free(file_name1);
    free(file_name2);
    free(merged_file_name);
    free(lock1);
    free(lock2);
    aot_merge_context_free(&ctx);
    return result;
exit_locked:
    free(file_name1);
    free(file_name2);
    free(merged_file_name);
    free(lock1);
    free(lock2);
    aot_merge_context_free(&ctx);
    return -1;
}

int aot_merge_all(const char *file_name, uint32_t aot_mode)
{
    int merged_count = 0;
    int rc;
    while ((rc = aot_merge_file(file_name, aot_mode)) > 0) {
        merged_count++;
    }
    if (rc == -3) {
        return -3;
    }
    if (rc == -1) {
        aot_verbose_log("[AOT][Merge] Deferred: %s locked, %d files merged\n",
                file_name, merged_count);
        return -1;
    }
    if (rc < 0) {
        fprintf(stderr, "[AOT] Merge failed: %d files merged for %s\n",
                merged_count, file_name);
        return rc;
    }
    return merged_count;
}

static void aot_merge(void)
{
    aot_merge_all("sys-aot-core", AOT_CORE);
    aot_merge_all("sys-aot-user", AOT_USER);
}

void aot_merger_drain(void)
{
    aot_merge();
}
