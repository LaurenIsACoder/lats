#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

import pathlib
import sys


root = pathlib.Path(__file__).resolve().parents[4]
save_source = (root / "target/i386/latx/sbt/sys-aot-save.c").read_text()
region_source = (root / "tcg/region.c").read_text()


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
                     "static void aot_reset_translation_buffers",
                     "static void aot_reclaim_translation_buffers")
if not reset:
    errors.append("AOT child translation-buffer reset helper is missing")
elif "tcg_region_rewind_current(s);" not in reset:
    errors.append("AOT child does not use the region-aware rewind helper")
elif "qatomic_set(&s->code_gen_ptr" in reset:
    errors.append("AOT child still rewinds the code pointer manually")

rewind = source_block(region_source,
                      "void tcg_region_rewind_current",
                      "extern uint64_t get_region_offset")
if not rewind:
    errors.append("TCG current-region rewind helper is missing")
elif "tcg_region_assign(s, curr_region, skip_prologue);" not in rewind:
    errors.append("TCG rewind does not reuse region boundary assignment")
elif "option_region_prologue || curr_region == base_region" not in rewind:
    errors.append("TCG rewind does not preserve the applicable prologue")
elif "qemu_mutex_lock" in rewind:
    errors.append("fork-child rewind unexpectedly acquires a region mutex")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)

print("PASS: post-fork AOT rewind preserves the current region prologue")
