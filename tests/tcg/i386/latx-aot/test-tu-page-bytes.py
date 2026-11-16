#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise production TU dependency lookup with colliding page hashes."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
header = (root / "include/lats/aot-format.h").read_text()


def function(signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define TARGET_PAGE_SIZE 4096
#define AOT_TB_MAGIC 1
#define AOT_TU_REF_MAGIC 2
static uint32_t aot_hash_size = 16;
static unsigned option_arch_opt;

static void *in_tb_ptr;
/* Force collisions: byte comparison and ownership must decide acceptance. */
static uint32_t get_page_hash(uint32_t *p) { (void)p; return 0; }
'''
for name in ("page_head", "aot_tu_head", "aot_tu_ref"):
    start = header.index("typedef struct " + name + " {")
    end = header.index("} " + name + ";", start) + len("} " + name + ";")
    prefix += header[start:end] + "\n"
index_header = (root / "target/i386/latx/sbt/sys-aot-internal.h").read_text()
start = index_header.index("typedef struct AOTPageIndexEntry {")
end = index_header.index("} AOTPageIndexEntry;", start) + len("} AOTPageIndexEntry;")
prefix += index_header[start:end] + "\nstatic AOTPageIndexEntry hash_table[16];\n"
prefix += function("static bool aot_page_contains_tu_dependency(")
prefix += function("static bool aot_tu_dependency_matches(")
prefix += r'''
typedef struct {
    page_head head;
    uint8_t bytes[TARGET_PAGE_SIZE];
    union { aot_tu_head tu; aot_tu_ref ref; } record;
} FilePage;
static FilePage file[9];
int main(void)
{
    for(option_arch_opt=0;option_arch_opt<2;option_arch_opt++) {
    uint8_t ram[TARGET_PAGE_SIZE] = {0};
    in_tb_ptr = file;
    aot_tu_head *tu = &file[0].record.tu;
    tu->tb_magic = AOT_TB_MAGIC;
    tu->tu_id = 123;
    tu->page_count = 8;
    tu->tu_sum_size = sizeof(*tu);
    for (int i = 0; i < 16; i++) { hash_table[i].offset = UINT64_MAX; }
    for (int i = 0; i < 9; i++) {
        file[i].head.tb_count = 1;
        if (i) {
            file[i].record.ref = (aot_tu_ref){
                .ref_magic = AOT_TU_REF_MAGIC, .tu_id = 123,
                .anchor_delta = -i * 4096LL
            };
        }
    }
    /* Equal bytes and hash belonging to a different TU precede real pages. */
    file[8].record.ref.tu_id = 999;
    hash_table[0].offset = 8 * sizeof(FilePage);
    for (int i = 0; i < 8; i++) { hash_table[i + 1].offset = i * sizeof(FilePage); }
    for (int i = 0; i < 8; i++) {
        int64_t delta = i * 4096LL;
        assert(aot_tu_dependency_matches(ram, tu, delta));
        /* Every byte participates, even though all sampled hashes collide. */
        for (int byte = 0; byte < TARGET_PAGE_SIZE; byte++) {
            file[i].bytes[byte] = 1;
            assert(!aot_tu_dependency_matches(ram, tu, delta));
            file[i].bytes[byte] = 0;
        }
    }
    file[1].record.ref.tu_id++;
    assert(!aot_tu_dependency_matches(ram, tu, 4096));
    file[1].record.ref.tu_id--;
    file[1].record.ref.anchor_delta = 4096;
    assert(!aot_tu_dependency_matches(ram, tu, 4096));
    file[1].record.ref.anchor_delta = -4096;
    /* A second payload with the same ID is not the anchor we selected. */
    file[8].record.tu = *tu;
    hash_table[1].offset = 8 * sizeof(FilePage);
    assert(!aot_tu_dependency_matches(ram, tu, 0));
    hash_table[1].offset = 0;
    assert(aot_tu_dependency_matches(ram, tu, 0));
    /* Changed RAM after an earlier match must never reuse a cached result. */
    ram[17] = 1;
    assert(!aot_tu_dependency_matches(ram, tu, 4096));
    ram[17] = 0;
    assert(aot_tu_dependency_matches(ram, tu, 4096));
    /* A full collision chain remains bounded when no dependency matches. */
    for (int i = 0; i < 16; i++) { hash_table[i].offset = 8 * sizeof(FilePage); }
    assert(!aot_tu_dependency_matches(ram, tu, 4096));
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="aot-tu-bytes-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(prefix)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror",
                    *shlex.split(os.environ.get("CFLAGS", "")),
                    str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: every dependency byte and TU ownership checked "
      "despite hash collisions")
