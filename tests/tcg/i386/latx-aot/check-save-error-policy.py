#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

from pathlib import Path
import re
import sys


root = Path(__file__).resolve().parents[4]
save_source = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()
common_source = (root / "target/i386/latx/sbt/sys-aot-common.c").read_text()
load_source = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
tu_source = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
aot_header = (root / "target/i386/latx/include/sys-aot.h").read_text()
errors = []


def source_block(source, start, end):
    begin = source.find(start)
    if begin < 0:
        return None
    finish = source.find(end, begin)
    return None if finish < 0 else source[begin:finish]


for source, name in (
    (save_source, "AOT save"),
    (common_source, "AOT common"),
    (load_source, "AOT load"),
    (tu_source, "TU translation"),
    (aot_header, "AOT interface"),
):
    if re.search(r"\bin_fork_child\b", source):
        errors.append(f"{name} still depends on in_fork_child")

for start, end, name in (
    ("char *gen_file_lock", "static int file_exists", "lock-name allocation"),
    ("FILE *aot_file_writer_open", "void aot_file_writer_abort",
     "file-writer open"),
):
    block = source_block(common_source, start, end)
    if not block:
        errors.append(f"{name} block is missing")
        continue
    if re.search(r"\b_?exit\s*\(", block):
        errors.append(f"{name} still terminates the process")
    if "return NULL;" not in block:
        errors.append(f"{name} does not return failure")

fatal = source_block(save_source, "static void aot_save_fatal",
                     "static void aot_save_context_reset_tb")
if not fatal or "_exit(EXIT_FAILURE);" not in fatal:
    errors.append("AOT save failures do not terminate with direct _exit")

generate = source_block(save_source, "static void generate_aot_file",
                        "static pid_t aot_runtime_save_get_child_pid")
if not generate:
    errors.append("AOT file generation block is missing")
elif "aot_file_writer_abort(&writer);" not in generate:
    errors.append("failed AOT generation does not abort its temporary writer")
elif "aot_save_fatal();" not in generate:
    errors.append("failed AOT generation does not terminate the save process")

child = source_block(save_source, "static void aot_save_child_main",
                     "static pid_t aot_start_save_child")
if (not child or
        "_exit(AOT_CHILD_EXIT_BASE | generated | merge_status);" not in child or
        "goto merge;" not in child):
    errors.append("AOT save child does not report completion by direct exit")
elif "get_tb(ctx, job->trigger.mtcc_id, true)" not in child:
    errors.append("runtime TB collection is not performed in the child")

flush_start = save_source.find("void aot_save_on_tb_flush")
flush = None if flush_start < 0 else save_source[flush_start:]
if not flush:
    errors.append("runtime AOT flush hook is missing")
elif "get_tb(" in flush:
    errors.append("runtime AOT flush still collects TBs before fork")

if "void aot_save_on_shutdown(void);" not in aot_header:
    errors.append("synchronous AOT shutdown interface has unexpected status")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)

print("PASS: AOT save processes report completion by direct child exit")
