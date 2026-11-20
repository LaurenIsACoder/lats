#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check the search-data byte count written by the shared AOT encoder."""

from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-tu-codec.c").read_text()
assignment = re.search(r"aot_tb->search_size\s*=.*?;", source, re.S)
assert assignment
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    unsigned icount;
    union { uintptr_t search_size; void *search_ptr; };
} TB;
static bool option_split_search;
static void encode(TB *aot_tb, TB *tb)
{
''' + assignment.group() + r'''
}
int main(void)
{
    TB tb = { .icount = 17 }, out = { 0 };
    int search[17];
    tb.search_ptr = search;
    option_split_search = true;
    encode(&out, &tb);
    assert(out.search_size == sizeof(search));
    tb.search_size = 23;
    option_split_search = false;
    encode(&out, &tb);
    assert(out.search_size == 23);
}
'''
with tempfile.TemporaryDirectory(prefix="aot-codec-search-") as directory:
    executable = Path(directory) / "test"
    subprocess.run(["cc", "-x", "c", "-Wall", "-Werror", "-o",
                    str(executable), "-"], input=program, text=True, check=True)
    subprocess.run([str(executable)], check=True)
print("PASS: shared AOT encoder search size with split search on/off")
