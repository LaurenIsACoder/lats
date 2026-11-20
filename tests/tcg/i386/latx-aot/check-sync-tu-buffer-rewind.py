#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

import pathlib
import sys


root = pathlib.Path(__file__).resolve().parents[4]
save_source = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()


def source_block(source, start, end):
    begin = source.find(start)
    if begin < 0:
        return None
    finish = source.find(end, begin)
    if finish < 0:
        return source[begin:]
    return source[begin:finish]


errors = []
reset = source_block(save_source,
                     "static void aot_reset_translation_buffers(void)\n{",
                     "void aot_prepare_merge_translation")
if not reset or "tcg_region_rewind_current(s);" not in reset:
    errors.append("offline save does not synchronously rewind its code region")
elif "tb_flush_mtcc(offline_cpu," in reset:
    errors.append("offline save still relies on asynchronous vCPU flush work")
elif "region_reset_tb(" in reset:
    errors.append("offline save resets the allocator holding collected templates")

prepare = source_block(save_source, "static void aot_prepare_save_cpu(void)",
                       "static void aot_reset_translation_buffers(void)\n{")
reclaim = source_block(save_source,
                       "static void aot_reclaim_translation_buffers(void)",
                       "static bool aot_page_writer_begin")
for name, block in (("prepare", prepare), ("reclaim", reclaim)):
    if not block or "aot_reset_translation_buffers();" not in block:
        errors.append(f"offline {name} does not call synchronous reset")
    elif "aot_runtime_save_child" in block:
        errors.append(f"offline {name} still restricts reset to fork children")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)

print("PASS: offline AOT save synchronously rewinds scratch buffers")
