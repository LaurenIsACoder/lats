#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

from pathlib import Path
import re
import sys


root = Path(__file__).resolve().parents[4]
tu_source = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
errors = []


def source_block(source, start, end):
    begin = source.find(start)
    if begin < 0:
        return None
    finish = source.find(end, begin)
    return None if finish < 0 else source[begin:finish]


fallback = source_block(
    tu_source,
    "if (explore_result == TU_EXPLORE_FALLBACK || unlikely(entry == NULL))",
    "if (explore_result == TU_EXPLORE_EXISTED)",
)
if not fallback:
    errors.append("TU explore fallback block is missing")
else:
    if not re.search(
        r"entry = tb_gen_code\(.*?\);.*?"
        r"if \(!check_misc_flags\(entry, MF_RAS_LINKED\)\) \{.*?"
        r"latxs_ras_unlink\(entry\);.*?\}",
        fallback,
        re.S,
    ):
        errors.append(
            "TU fallback does not preserve an ordinary-TB RAS link")

    guard = fallback.find(
        "if (!check_misc_flags(entry, MF_RAS_LINKED))")
    unlink = fallback.find("latxs_ras_unlink(entry)")
    if guard < 0 or unlink < guard:
        errors.append("TU fallback unlinks a published RAS target unconditionally")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)

print("PASS: TU fallback preserves ordinary-TB software-RAS links")
