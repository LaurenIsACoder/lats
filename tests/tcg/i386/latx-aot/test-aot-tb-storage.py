#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check the production offline clone allocation against inline TU storage."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
start = source.index("static TranslationBlock* aot_tb_create(")
clone = source[start:source.index("\n}", start)]
start = re.search(r"    tb = g_(?:new|malloc0)\(", clone).start()
end = clone.index("    if (recorded", start)
allocation = clone[start:end]

harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
struct separated_data { unsigned char metadata[128]; };
typedef struct TranslationBlock {
    uintptr_t pc;
    struct separated_data *s_data;
    unsigned char padding[368];
} TranslationBlock;
struct tb_tu_link_data { uintptr_t pages[2], generation[2]; };
static int link_check;
static size_t allocated_size;
static void *allocation_base;
static int tucp_fast_link_check(void) { return link_check; }
static void *checked_alloc(size_t size)
{
    assert(!allocation_base);
    allocation_base = calloc(1, size + 16);
    assert(allocation_base);
    allocated_size = size;
    memset((char *)allocation_base + size, 0xa5, 16);
    return allocation_base;
}
#define g_malloc0(size) checked_alloc(size)
#define g_new(type, count) ((type *)checked_alloc(sizeof(type) * (count)))
int main(void)
{
    for (link_check = 0; link_check <= 1; link_check++) {
        TranslationBlock tu_tb_tmp = { .pc = 0x1234 }, *tb;
        struct separated_data tmp_s_data;
        memset(&tmp_s_data, 0x3c, sizeof(tmp_s_data));
'''
harness += allocation
harness += r'''
        size_t minimum = sizeof(*tb) + sizeof(*tb->s_data);
        assert(tb == allocation_base && tb->pc == tu_tb_tmp.pc);
        assert(tb->s_data == (void *)(tb + 1));
        assert(!memcmp(tb->s_data, &tmp_s_data, sizeof(tmp_s_data)));
        if (link_check) {
            struct tb_tu_link_data *links = (void *)((char *)tb + minimum);
            minimum += sizeof(*links);
            assert(allocated_size >= minimum);
            memset(links, 0, sizeof(*links));
            links->pages[1] = 0x5678;
            links->generation[1] = 42;
        }
        assert(allocated_size == minimum);
        for (int i = 0; i < 16; i++) {
            assert(((unsigned char *)tb)[allocated_size + i] == 0xa5);
        }
        free(tb);
        allocation_base = NULL;
    }
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="aot-tb-storage-") as directory:
    path = Path(directory)
    cfile, binary = path / "test.c", path / "test"
    cfile.write_text(harness)
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-std=c11", "-Wall", "-Wextra", "-Werror", str(cfile),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: offline TB sidecar and optional TU link storage stay in bounds")
