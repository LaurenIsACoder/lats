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
deal_page = source_block(save_source,
                         "static bool deal_tu_page",
                         "static bool save_tu_cross_page")
if not deal_page:
    errors.append("single-page AOT TU save helper is missing")
else:
    pressure_check = "tcg_ctx->code_gen_highwater - tcg_ctx->code_gen_ptr"
    if pressure_check not in deal_page:
        errors.append("AOT TU save no longer checks code-cache pressure")
    if "region_reset_search(mtcc_id);" not in deal_page:
        errors.append("AOT TU save does not rewind auxiliary data per page")
    if deal_page.find("region_reset_search(mtcc_id);") < \
            deal_page.find(pressure_check):
        errors.append("auxiliary rewind is still tied to code-cache pressure")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)

print("PASS: synchronous AOT TU save rewinds auxiliary data per page")
