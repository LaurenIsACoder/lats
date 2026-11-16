#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Compile production TU candidate and return predicates; reject flag mismatches."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
header = (root / "include/exec/translation-block.h").read_text()


def function(signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def return_predicate(signature, assignment):
    body = function(signature)
    end = body.index(assignment)
    start = body.rfind("if (", 0, end) + len("if (")
    stop = body.rfind(") {", start, end)
    assert start >= len("if (") and stop > start
    return body[start:stop]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define TARGET_PAGE_MASK (~4095ULL)
typedef uint64_t vaddr;
typedef struct {
    vaddr pc;
    uint64_t cs_base;
    uint32_t flags, cflags;
} aot_block;
typedef aot_block TranslationBlock;
typedef struct {
    vaddr first_pc;
    uint32_t tb_count;
    aot_block blocks[2];
} aot_tu_head;
static aot_block *aot_tu_blocks(aot_tu_head *head) { return head->blocks; }
'''
for name in ("CF_NO_ACTIVE", "CF_INVALID", "CF_SINGLE_STEP", "CF_PARALLEL"):
    prefix += re.search(r"^#define " + name + r"\s+[^\n]+", header, re.M)[0] + "\n"
prefix += function("static inline vaddr aot_tu_rebase_pc(")
prefix += function("static bool aot_tu_matches(")
for name, signature, assignment, arg in (
    ("loaded_matches", "TranslationBlock *load_tu(", "ret = linked;", "tb"),
    ("returned_matches", "TranslationBlock *load_page_tu(",
     "ret_tb = curr_tb;", "curr_tb"),
):
    predicate = return_predicate(signature, assignment)
    prefix += f"""
static bool {name}(TranslationBlock *{arg}, vaddr pc,
                  uint32_t flags, int cflags)
{{ return {predicate}; }}
"""

main = r'''
int main(void)
{
    aot_tu_head head = { .first_pc = 0x1000, .tb_count = 2 };
    uint32_t flags = 0x123;
    int cflags = CF_PARALLEL;
    const uint32_t mismatches[] = {
        CF_NO_ACTIVE, CF_INVALID, CF_NO_ACTIVE | CF_INVALID,
        CF_SINGLE_STEP, CF_PARALLEL
    };
    /* Exercise anchor-page and cross-page members. */
    for (int member = 0; member < 2; member++) {
        for (unsigned variant = 0; variant <= 5; variant++) {
            head.blocks[0] = (aot_block){ .pc = 0x1010, .flags = flags,
                                        .cflags = cflags };
            head.blocks[1] = (aot_block){ .pc = 0x2010, .flags = flags,
                                        .cflags = cflags };
            if (variant) {
                head.blocks[member].cflags ^= mismatches[variant - 1];
            }
            vaddr pc = 0x8010 + member * 4096;
            assert(aot_tu_matches(&head, 0x8000, pc, 0, flags, cflags)
                   == (variant == 0));
            TranslationBlock tb = head.blocks[member];
            tb.pc = pc;
            assert(loaded_matches(&tb, pc, flags, cflags) == (variant == 0));
            assert(returned_matches(&tb, pc, flags, cflags) == (variant == 0));
            assert(!loaded_matches(&tb, pc + 1, flags, cflags));
            assert(!returned_matches(&tb, pc, flags + 1, cflags));
        }
    }
    assert(!returned_matches(NULL, 0, flags, cflags));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="aot-tu-cflags-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(prefix + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                    "-Wextra", "-Werror", "-Wno-sign-compare",
                    *shlex.split(os.environ.get("CFLAGS", "")),
                    str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: TU candidates and returned TBs require exact cflags")
