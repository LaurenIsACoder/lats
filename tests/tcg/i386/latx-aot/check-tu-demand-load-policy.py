#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile


root = Path(__file__).resolve().parents[4]
load_source = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
maint_source = (root / "accel/tcg/tb-maint.c").read_text()
errors = []


def source_block(source, start, end):
    begin = source.find(start)
    if begin < 0:
        return None
    finish = source.find(end, begin)
    return None if finish < 0 else source[begin:finish]


page_load = source_block(load_source,
                         "static TranslationBlock *aot_try_load_tu(",
                         "static TranslationBlock *aot_load_tu_ref(")
if not page_load:
    errors.append("AOT TU page-load block is missing")
else:
    select_pos = page_load.find("aot_tu_matches(")
    load_pos = page_load.find("TranslationBlock *tb = load_tu(")
    if select_pos < 0 or load_pos < 0 or select_pos > load_pos:
        errors.append("AOT TU load is not gated by a target-TB match")

page_find = source_block(load_source, "static uint64_t find_aot_tu_page(",
                         "static TranslationBlock *activate_tb(")
if not page_find:
    errors.append("AOT TU page-cache lookup block is missing")
else:
    for token, description in (
        ("cache.file_generation == aot_file_generation",
         "published-file generation check"),
        ("find_page(hva)", "full lookup after cache invalidation"),
        ("cache.file_offset", "cached page-file offset"),
        ("set_page_aot_tu_cache", "page validation cache update"),
    ):
        if token not in page_find:
            errors.append(f"AOT TU page cache lacks {description}")

invalidate = source_block(
    maint_source,
    "tb_invalidate_phys_page_range__locked(struct page_collection *pages,",
    "TranslationBlock *tb;",
)
if not invalidate:
    errors.append("physical-page invalidation block is missing")
else:
    for field in (
        "aot_tu_file_offset = 0",
        "aot_tu_cache_generation = 0",
    ):
        if field not in invalidate:
            errors.append(f"SMC invalidation does not reset {field}")

if "aot_tu_page_hash" in maint_source or "cache.page_hash" in load_source:
    errors.append("page lookup cache still stores a page hash")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)

def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


# Exercise production cache helpers, with the full page lookup counted.
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define g_assert assert
typedef uint64_t tb_page_addr_t;
typedef uintptr_t ADDR;
typedef struct {
    uint64_t file_offset;
    uint32_t file_generation;
} AOTTUPageCache;
typedef struct {
    uint64_t aot_tu_file_offset;
    uint32_t aot_tu_cache_generation;
} PageDesc;
static PageDesc pages[2];
static bool allocated[2];
static uint32_t aot_file_generation = 1;
static uintptr_t in_tb_ptr = 0x10000;
static unsigned lookups;
static uint64_t lookup_result;
static PageDesc *page_find(uint64_t index)
{
    assert(index < 2);
    return allocated[index] ? &pages[index] : NULL;
}
static PageDesc *page_find_alloc(uint64_t index, bool alloc)
{
    assert(index < 2 && alloc);
    allocated[index] = true;
    return &pages[index];
}
static uint64_t find_page(ADDR hva)
{
    assert(hva == 0x4000);
    lookups++;
    return lookup_result;
}
'''
harness += function(maint_source, "bool get_page_aot_tu_cache(")
harness += function(maint_source, "void set_page_aot_tu_cache(")
harness += function(load_source, "static uint64_t find_aot_tu_page(")
harness += r'''
int main(void)
{
    AOTTUPageCache cache;
    assert(!get_page_aot_tu_cache(0, &cache));
    lookup_result = in_tb_ptr + 64;
    assert(find_aot_tu_page(0x4000, 0) == lookup_result && lookups == 1);
    assert(find_aot_tu_page(0x4000, 0) == lookup_result && lookups == 1);
    /* A new mapping of the AOT file must not reuse an old offset. */
    aot_file_generation++;
    in_tb_ptr += 0x10000;
    lookup_result = in_tb_ptr + 128;
    assert(find_aot_tu_page(0x4000, 0) == lookup_result && lookups == 2);
    /* Negative results are cached too, but expire on file publication. */
    lookup_result = 0;
    assert(find_aot_tu_page(0x4000, 1) == 0 && lookups == 3);
    assert(find_aot_tu_page(0x4000, 1) == 0 && lookups == 3);
    aot_file_generation++;
    lookup_result = in_tb_ptr + 256;
    assert(find_aot_tu_page(0x4000, 1) == lookup_result && lookups == 4);
    /* Model the SMC reset checked above: even the same file needs lookup. */
    pages[1].aot_tu_file_offset = 0;
    pages[1].aot_tu_cache_generation = 0;
    assert(!get_page_aot_tu_cache(1, &cache));
    lookup_result = 0;
    assert(find_aot_tu_page(0x4000, 1) == 0 && lookups == 5);
    assert(find_aot_tu_page(0x4000, 1) == 0 && lookups == 5);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="aot-page-cache-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")),
                    str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

print("PASS: demand loading caches lookup results and revalidates after SMC/publication")
