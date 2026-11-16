#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check save logging after tail cleanup, including failed commits."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()


def function(signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#define CONFIG_LATX_TU 1
#define AOT_CORE 1
#define AOT_USER 2
typedef int TranslationBlock;
typedef struct {
    void *page_info_list;
    uint32_t page_info_capacity, page_info_count;
    uint64_t output_tb_count;
    TranslationBlock **core_tb_vector, **user_tb_vector;
} AOTSaveContext;
typedef struct { const char *final_name, *tmp_name; } AOTFileWriter;
static const char *aot_file_path = "/unused";
static int option_verbose, option_sys_aot = 1, option_aot_cross_page_tu;
static unsigned option_tu_mode;
#define LATX_TU_MODE_JIT (1u << 0)
#define LATX_TU_MODE_AOT (1u << 1)
static inline bool jit_tu_enabled(void)
{ return option_tu_mode & LATX_TU_MODE_JIT; }
static inline bool aot_tu_requested(void)
{ return option_tu_mode & LATX_TU_MODE_AOT; }
static inline bool aot_tu_enabled(void)
{ return option_sys_aot && aot_tu_requested(); }
static bool fail_commit, fail_tail;
static uint32_t pages;
static uint64_t tbs;
static void aot_file_writer_init(AOTFileWriter *w, const char *path,
                                 const char *name)
{ (void)path; w->final_name = name; w->tmp_name = "unused.tmp"; }
static bool aot_file_writer_open(AOTFileWriter *w) { (void)w; return true; }
static bool aot_file_writer_commit(AOTFileWriter *w)
{ (void)w; return !fail_commit; }
static void aot_file_writer_abort(AOTFileWriter *w) { (void)w; }
static void aot_save_fatal(void) { exit(1); }
static bool save_tb_by_page(AOTSaveContext *ctx, TranslationBlock **tb,
                            uint32_t count, uint32_t mode)
{
    (void)tb; (void)count; (void)mode;
    ctx->page_info_count = pages;
    ctx->output_tb_count = tbs;
    return true;
}
#define save_tu_by_page save_tb_by_page
static bool save_tu_cross_page(AOTSaveContext *ctx, TranslationBlock **tb,
                               uint32_t count)
{ return save_tb_by_page(ctx, tb, count, AOT_USER); }
'''
prefix += function("static void aot_save_context_reset_page_info(")
prefix += function("static void aot_save_context_free_page_info(")
prefix += r'''
static bool aot_save_page_info_tail(AOTSaveContext *ctx, uint32_t mode)
{
    (void)mode;
    fprintf(stderr, "TAIL pages=%u, TBs=%" PRIu64 "\n",
            ctx->page_info_count, ctx->output_tb_count);
    aot_save_context_free_page_info(ctx);
    return !fail_tail;
}
'''
# Ensure the modeled tail cleanup still exists in the production tail writer.
assert "aot_save_context_free_page_info(ctx);" in function(
    "static bool aot_save_page_info_tail(")
prefix += function("static void generate_aot_file(")
prefix += r'''
int main(int argc, char **argv)
{
    AOTSaveContext ctx = {0};
    if (argc != 4) { return 2; }
    int mode = atoi(argv[1]);
    option_tu_mode = mode != 0 ? LATX_TU_MODE_AOT : 0;
    option_aot_cross_page_tu = mode == 2;
    fail_commit = atoi(argv[2]) == 1;
    fail_tail = atoi(argv[2]) == 2;
    pages = atoi(argv[3]) ? 0 : 7;
    tbs = pages ? 23 : 0;
    generate_aot_file(&ctx, "result", 100, mode ? AOT_USER : AOT_CORE);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="aot-save-counts-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(prefix)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", str(cfile), "-o", str(binary)],
                   check=True)
    for mode in range(3):
        for failure in range(3):
            for empty in range(2):
                result = subprocess.run([str(binary), str(mode), str(failure),
                                         str(empty)], capture_output=True,
                                        universal_newlines=True)
                assert result.returncode == bool(failure), result.stderr
                committed = [line for line in result.stderr.splitlines()
                             if "Committed:" in line]
                if failure:
                    assert not committed, result.stderr
                else:
                    expected = "pages=0, TBs=0" if empty else "pages=7, TBs=23"
                    assert len(committed) == 1, result.stderr
                    assert committed[0].endswith(expected), result.stderr
print("PASS: committed counts survive cleanup; failures never report success")
