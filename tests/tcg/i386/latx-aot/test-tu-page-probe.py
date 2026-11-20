#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Compile the real candidate-page probe against a fault-injecting MMU."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[4]
path = "target/i386/latx/sbt/sys-aot-load.c"
source = (
    subprocess.check_output(["git", "show", ":" + path], cwd=root).decode()
    if "--index" in sys.argv else (root / path).read_text())
start = source.index("static tb_page_addr_t aot_probe_code_page(")
end = source.index("\n}", start) + 2
probe = source[start:end]
for begin, finish in (
    ("static uint64_t aot_find_tu_page_record(",
     "static TranslationBlock *activate_tb("),
    ("static bool aot_lock_tu_dependencies(",
     "static TranslationUnitPages *aot_build_tu_pages_desc("),
):
    body = source[source.index(begin):source.index(finish)]
    assert "get_page_addr_code_hostp(" not in body
    assert "aot_probe_code_page(" in body

prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef uint64_t vaddr;
typedef uint64_t tb_page_addr_t;
typedef struct {
    int error_code;
    uint64_t cr2;
    int exception_index;
} CPUArchState;
static int fail;
static tb_page_addr_t get_page_addr_code_hostp_noexit(CPUArchState *env,
        vaddr pc, void **hostp)
{
    if (fail) {
        env->error_code = 99;
        return -1;
    }
    if (hostp) { *hostp = (void *)(uintptr_t)(pc + 0x1000); }
    return pc + 0x1000;
}
'''
main = r'''
int main(void)
{
    CPUArchState env = { 17, 0x1234, 23 };
    void *hostp = NULL;
    assert(aot_probe_code_page(&env, 0x2000, &hostp) == 0x3000);
    assert(hostp == (void *)0x3000);
    fail = 1;
    assert(aot_probe_code_page(&env, 0x4000, NULL) == (uint64_t)-1);
    assert(env.error_code == 17 && env.cr2 == 0x1234 &&
           env.exception_index == 23);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="aot-page-probe-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(prefix + probe + main)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11",
                    "-Wall", "-Wextra",
                    "-Werror", str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: candidate-page probes reject missing pages "
      "without guest exceptions")
