#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise entry-evidence union in the actual saved-TU deduplicator."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/merge.c").read_text()
begin = source.index("static const aot_tu_page *aot_merge_tu_pages(")
end = source.index("static bool aot_merge_tu_records(", begin)
test = r'''
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lats/aot-format.h"
#define TARGET_PAGE_MASK (~UINT64_C(4095))
typedef uint64_t vaddr;
typedef struct AOTMergeContext {
    FILE *out_file;
    bool write_err;
    int reduction_tb_count, merged_tb_count;
} AOTMergeContext;
static bool aot_merge_write(AOTMergeContext *ctx, const void *p, size_t n)
{
    return fwrite(p, 1, n, ctx->out_file) == n;
}
'''
test += source[begin:end]
saved = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()
test += """
typedef uint64_t target_ulong, tb_page_addr_t;
#define AOT_TU_MAPPING_RESULT_COUNT 4
#define MF_OBSERVED_CALL_ENTRY AOT_BLOCK_OBSERVED_CALL_ENTRY
"""
test += saved[saved.index("typedef struct AOTTUSaveRef {"):
              saved.index("#define AOT_TU_COPY_BUFFER_SIZE")]
test += saved[saved.index("static bool aot_spool_merge_call_entries("):
              saved.index("static bool save_tu_cross_page(")]
test += r'''
#define HF_CPL_MASK 3
#define qatomic_read(p) (*(p))
#define qatomic_or(p, v) (*(p) |= (v))
typedef struct TranslationBlock {
    uint64_t pc, hva, cs_base;
    uint32_t flags, cflags, misc_flags;
} TranslationBlock;
#define tb_page_addr0(tb) ((tb)->hva | ((tb)->pc & 4095))
typedef struct AOTSaveContext {
    TranslationBlock **tb_vector, **core_tb_vector, **user_tb_vector;
    int tb_num, core_tb_num, user_tb_num;
} AOTSaveContext;
'''
test += saved[saved.index("static int tb_cmp("):
              saved.index("static inline bool not_save(")]
test += "static void deduplicate(AOTSaveContext *ctx) {\n"
test += saved[saved.index("    qsort(ctx->tb_vector,"):
              saved.index('    aot_verbose_log("[AOT][Save] Selected:')]
test += "}\n"
test += r'''
struct Record {
    aot_tu_head head;
    aot_tu_page page;
    aot_block blocks[2];
};
int main(void)
{
    struct Record first = {0}, other, result;
    first.head = (aot_tu_head) {
        .tb_magic = AOT_TB_MAGIC, .tu_sum_size = sizeof(first),
        .tu_id = 7, .first_pc = 0x1000, .page_count = 1, .tb_count = 2,
    };
    first.blocks[0].pc = 0x1000;
    first.blocks[1].pc = 0x1008;
    first.blocks[0].misc_flags = 0x8000;
    other = first;
    other.head.first_pc += 0x100000;
    for (int i = 0; i < 2; i++) {
        other.blocks[i].pc += 0x100000;
    }
    AOTMergeContext ctx = {.out_file = tmpfile()};
    AOTWrittenTURecord written[2];
    uint32_t count = 0;
    page_head page = {0};
    assert(ctx.out_file);
    assert(fwrite("prefix", 1, 6, ctx.out_file) == 6);
    assert(aot_write_unique_tu_record(&ctx, &first, written, &count, &page));
    for (int i = 0; i < 2; i++) {
        other.blocks[0].misc_flags = 0;
        other.blocks[1].misc_flags = 0;
        other.blocks[i].misc_flags = AOT_BLOCK_OBSERVED_CALL_ENTRY | 0x4000;
        assert(aot_write_unique_tu_record(&ctx, &other, written, &count, &page));
    }
    assert(count == 1 && page.tb_count == 1 && ctx.reduction_tb_count == 4);
    assert(ftell(ctx.out_file) == 6 + sizeof(first));
    rewind(ctx.out_file);
    char prefix[6];
    assert(fread(prefix, 1, 6, ctx.out_file) == 6);
    assert(!memcmp(prefix, "prefix", 6));
    assert(fread(&result, sizeof(result), 1, ctx.out_file) == 1);
    first.blocks[0].misc_flags |= AOT_BLOCK_OBSERVED_CALL_ENTRY;
    first.blocks[1].misc_flags |= AOT_BLOCK_OBSERVED_CALL_ENTRY;
    assert(!memcmp(&result, &first, sizeof(first)));
    /* A changed source page is a different identity, even at the same PC. */
    other.page.page_fingerprint++;
    assert(aot_write_unique_tu_record(&ctx, &other, written, &count, &page));
    assert(count == 2 && page.tb_count == 2 && !ctx.write_err);
    fclose(ctx.out_file);

    first.blocks[0].misc_flags = 0x8000;
    first.blocks[1].misc_flags = 0;
    other = first;
    other.head.first_pc += 0x100000;
    for (int i = 0; i < 2; i++) {
        other.blocks[i].pc += 0x100000;
    }
    AOTTUSpoolRecord records[2] = {
        {.file_offset = 0, .size = sizeof(first), .tb_count = 2, .tu_id = 7},
        {.file_offset = sizeof(first), .size = sizeof(other), .tb_count = 2, .tu_id = 7},
    };
    AOTCrossPageSpool spool = {.file = tmpfile(), .records = records};
    AOTTUSaveRef keep = {.tu_index = 0}, add = {.tu_index = 1};
    assert(spool.file && fwrite(&first, sizeof(first), 1, spool.file) == 1);
    for (int i = 0; i < 2; i++) {
        other.blocks[0].misc_flags = 0;
        other.blocks[1].misc_flags = 0;
        other.blocks[i].misc_flags = AOT_BLOCK_OBSERVED_CALL_ENTRY | 0x4000;
        assert(!fseek(spool.file, sizeof(first), SEEK_SET));
        assert(fwrite(&other, sizeof(other), 1, spool.file) == 1);
        assert(aot_spool_merge_call_entries(&spool, &keep, &add));
    }
    rewind(spool.file);
    assert(fread(&result, sizeof(result), 1, spool.file) == 1);
    first.blocks[0].misc_flags |= AOT_BLOCK_OBSERVED_CALL_ENTRY;
    first.blocks[1].misc_flags |= AOT_BLOCK_OBSERVED_CALL_ENTRY;
    assert(!memcmp(&result, &first, sizeof(first)));
    fclose(spool.file);

    TranslationBlock aliases[] = {
        {.pc = 0x1000, .hva = 0x100000, .flags = 3, .misc_flags = 0x8000},
        {.pc = 0x2000, .hva = 0x100000, .flags = 3,
         .misc_flags = MF_OBSERVED_CALL_ENTRY},
        {.pc = 0x3000, .hva = 0x200000, .flags = 3},
        {.pc = 0x4008, .hva = 0x100000, .flags = 3},
    };
    TranslationBlock *vector[] = {&aliases[0], &aliases[1], &aliases[2], &aliases[3]};
    AOTSaveContext selection = {.tb_vector = vector, .tb_num = 4};
    deduplicate(&selection);
    assert(selection.tb_num == 3 && selection.user_tb_num == 3);
    for (int i = 0; i < selection.tb_num; i++) {
        TranslationBlock *tb = vector[i];
        bool shared_entry = tb->hva == 0x100000 && !(tb->pc & 4095);
        assert(!!(tb->misc_flags & MF_OBSERVED_CALL_ENTRY) == shared_entry);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="aot-call-merge-") as directory:
    directory = Path(directory)
    path = directory / "test.c"
    path.write_text(test)
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11",
                    "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-I", root / "include",
                    path, "-o", directory / "test"], check=True)
    subprocess.run([directory / "test"], check=True)
print("PASS observed CALL evidence survives save-time and file-merge deduplication")
