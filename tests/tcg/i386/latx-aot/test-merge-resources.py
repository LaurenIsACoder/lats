#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Compile the production merge control flow with small I/O fixtures."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / 'target/i386/latx/sbt/merge.c').read_text()


def function(name):
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


context = re.search(r'typedef struct AOTMergeContext \{.*?\} AOTMergeContext;',
                    source, re.S).group()
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define aot_verbose_log(...) ((void)0)
#define TARGET_PAGE_SIZE 4096
#define AOT_PAGE_MAGIC 0x66666666
#define AOT_MERGED_MAGIC 0x77777777
#define AOT_USER 2
typedef struct {
    uint32_t page_magic, page_hash, tb_count;
    int curr_page_sum_size;
} page_head;
typedef struct { uint32_t page_hash; uint64_t file_offset; } page_info_t;
typedef struct { uint32_t page_count; page_info_t pages[]; } page_info_array_t;
typedef struct { int page_num, tb_num; } aot_tail;
static char *aot_file_path = "./";
static FILE *out_tb_file;
static unsigned option_tu_mode;
static int option_sys_aot = 1;
#define LATX_TU_MODE_JIT (1u << 0)
#define LATX_TU_MODE_AOT (1u << 1)
static inline bool aot_tu_requested(void)
{ return option_tu_mode & LATX_TU_MODE_AOT; }
static inline bool aot_tu_enabled(void)
{ return option_sys_aot && aot_tu_requested(); }
static int fail_input, load_calls, live_maps, unmaps, busy;
static bool fail_write;
static void *maps[2];
static size_t lengths[2];
static int checked_munmap(void *p, size_t n)
{
    int i;
    for (i = 0; i < 2; i++) { if (maps[i] == p) { break; } }
    assert(i < 2 && lengths[i] == n);
    maps[i] = NULL;
    live_maps--;
    unmaps++;
    return munmap(p, n);
}
#define munmap checked_munmap
'''
stubs = r'''
static void *aot_tu_merge_cpu;
static bool aot_merge_tu_files(const void *a, uint64_t as,
    const void *b, uint64_t bs, FILE *out, uint32_t mode, int *p, int *t)
{ (void)a; (void)as; (void)b; (void)bs; (void)out; (void)mode;
  (void)p; (void)t; abort(); }
static bool test_integrity(int fd, uint32_t mode)
{ (void)fd; (void)mode; abort(); }
static bool validate_aot_layout(void *p, uint64_t n)
{ (void)p; (void)n; abort(); }

static int find_first_add_file(const char *name)
{
    (void)name;
    return access("cache-add1", F_OK) == 0 ? 1 : 0;
}
static int file_lock(const char *name, int *fd, int type, bool block)
{
    (void)name; (void)fd; (void)type; (void)block;
    return busy ? -1 : 0;
}
static void aot_merge_close_fd(int *fd) { (void)fd; }
static void aot_merge_unlock_fd(int *fd) { (void)fd; }
static void *load_file_to_mem(char *name, uint64_t *size, uint32_t mode)
{
    (void)mode;
    int slot = load_calls++;
    if (slot == fail_input) { return NULL; }
    int fd = open(name, O_RDONLY);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    *size = st.st_size;
    void *p = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    assert(p != MAP_FAILED);
    maps[slot] = p;
    lengths[slot] = *size;
    live_maps++;
    return p;
}
static uintptr_t find_other_file_page(uintptr_t p, uint64_t n,
                                      uint32_t h, void *code)
{ (void)p; (void)n; (void)h; (void)code; return 0; }
static inline bool aot_cross_page_tu_enabled(void) { return true; }
static inline bool aot_count_page_tu_tbs(uintptr_t p, int *n)
{ (void)p; *n = 1; return true; }
static bool aot_page_info_size(int n, size_t *size)
{ *size = sizeof(page_info_array_t) + n * sizeof(page_info_t); return true; }
static bool save_aot_tail(int pages, int tbs, uint32_t mode)
{
    (void)tbs; (void)mode;
    aot_tail tail = { .page_num = pages };
    return fwrite(&tail, sizeof(tail), 1, out_tb_file) == 1;
}
static void save_same_page(AOTMergeContext *ctx, uintptr_t a, uintptr_t b)
{ (void)ctx; (void)a; (void)b; abort(); }
static inline void save_same_tu_page(AOTMergeContext *ctx, uintptr_t a, uintptr_t b)
{ save_same_page(ctx, a, b); }
static bool aot_merge_context_append_page_info(AOTMergeContext *ctx,
                                              uint32_t h, uint64_t offset)
{
    ctx->page_info_list = realloc(ctx->page_info_list,
        (ctx->page_info_count + 1) * sizeof(page_info_t));
    assert(ctx->page_info_list);
    ctx->page_info_list[ctx->page_info_count++] = (page_info_t) { h, offset };
    return true;
}
static size_t fixture_fwrite(const void *p, size_t n, size_t count, FILE *f)
{
    if (fail_write) { errno = ENOSPC; return 0; }
    return fwrite(p, n, count, f);
}
#define fwrite fixture_fwrite
'''
main = r'''
#undef fwrite
static void fixture(const char *name)
{
    FILE *f = fopen(name, "wb");
    assert(f);
    page_head ph = { AOT_PAGE_MAGIC, 7, 1, sizeof(ph) + TARGET_PAGE_SIZE };
    char data[TARGET_PAGE_SIZE] = { 0 };
    uint32_t count = 1;
    page_info_t info = { 7, 0 };
    /* Align the flexible array exactly as in the fixture struct. */
    char index[sizeof(page_info_array_t) + sizeof(info)];
    memset(index, 0, sizeof(index));
    memcpy(index, &count, sizeof(count));
    memcpy(index + sizeof(page_info_array_t), &info, sizeof(info));
    aot_tail tail = { .page_num = 1 };
    assert(fwrite(&ph, sizeof(ph), 1, f) == 1);
    assert(fwrite(data, sizeof(data), 1, f) == 1);
    assert(fwrite(index, sizeof(index), 1, f) == 1);
    assert(fwrite(&tail, sizeof(tail), 1, f) == 1);
    assert(fclose(f) == 0);
}
static void reset(void)
{
    assert(live_maps == 0);
    fail_input = -1; load_calls = unmaps = busy = 0; fail_write = false;
    fixture("cache"); fixture("cache-add1");
}
int main(void)
{
    for (int i = 0; i < 20; i++) {
        reset();
        assert(aot_merge_file("cache", 0) == 1);
        assert(live_maps == 0 && unmaps == 2);
        assert(access("cache-add1", F_OK) != 0);
    }
    for (int i = 0; i < 2; i++) {
        reset(); fail_input = i;
        assert(aot_merge_file("cache", 0) <= 0);
        assert(live_maps == 0 && unmaps == 1);
    }
    reset(); fail_write = true;
    assert(aot_merge_file("cache", 0) <= 0);
    assert(live_maps == 0 && unmaps == 2);
    assert(access("cache", F_OK) == 0 && access("cache-add1", F_OK) == 0);
    assert(access("cache-tmp", F_OK) != 0);
    reset(); busy = 1;
    assert(aot_merge_file("cache", 0) < 0);
    assert(live_maps == 0 && load_calls == 0);
    reset(); assert(unlink("cache-add1") == 0);
    assert(aot_merge_file("cache", 0) == 0 && load_calls == 0);
    AOTMergeContext empty = { 0 };
    aot_merge_context_free(&empty);
    aot_merge_context_free(&empty);
    return 0;
}
'''


def run(extra_helpers='', test_main=main):
    if 'static bool aot_merge_write(' in source:
        extra_helpers = function('static bool aot_merge_write') + extra_helpers
    bodies = '\n'.join(function(name) for name in (
        'static void aot_merge_context_free', 'static void save_one_page',
        'static int aot_merge_file'))
    with tempfile.TemporaryDirectory(prefix='aot-merge-test-') as directory:
        cfile = Path(directory) / 'test.c'
        binary = Path(directory) / 'test'
        cfile.write_text(
            prefix + context + stubs + extra_helpers + bodies + test_main)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall',
                        '-Wextra', '-Werror', '-Wno-sign-compare',
                        str(cfile), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], cwd=directory, check=True,
                              capture_output=True, text=True)


if __name__ == '__main__':
    run()
    print('PASS: repeated merges and partial failures '
          'release private input mappings')
