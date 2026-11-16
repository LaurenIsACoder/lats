#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check that offline AOT IR1 verification resolves every TU guest page."""

from __future__ import annotations

from pathlib import Path
import sys


root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/aot-verify-mode.c").read_text()
errors = []


def source_block(start: str, end: str) -> str | None:
    begin = source.find(start)
    if begin < 0:
        return None
    finish = source.find(end, begin)
    return None if finish < 0 else source[begin:finish]


page_index = source_block("static GHashTable *build_aot_guest_page_index(",
                          "static uint8_t *find_aot_tu_guest_page(")
if not page_index:
    errors.append("offline verifier guest-page index is missing")
elif "get_page_fingerprint(guest_code)" not in page_index:
    errors.append("offline verifier does not index pages by full fingerprint")

parse = source_block("static int parse_aot_data(",
                     "static aot_tb_entry *find_aot_tb(")
if not parse:
    errors.append("offline verifier AOT parser is missing")
else:
    for token, description in (
        ("pages[i].pc_delta == pc_delta", "TU dependency lookup"),
        ("(blocks[i].pc & TARGET_PAGE_MASK) -",
         "non-anchor TB page delta"),
        ("page_delta + TARGET_PAGE_SIZE", "second guest page lookup"),
    ):
        if token not in source:
            errors.append(f"offline verifier lacks {description}")

ir1 = source_block("static bool aot_ir1_matches(",
                   "static uint8_t aot_ir1_byte(")
if not ir1:
    errors.append("offline verifier IR1 comparison helper is missing")
else:
    for token, description in (
        ("TARGET_PAGE_SIZE - offset", "page-boundary split"),
        ("aot->guest_code[1]", "second-page comparison"),
        ("record->ir1_data + first_size", "second IR1 segment"),
    ):
        if token not in ir1:
            errors.append(
                f"offline verifier IR1 comparison lacks {description}")

if errors:
    print("\n".join(f"FAIL: {error}" for error in errors))
    sys.exit(1)

print("PASS: offline AOT IR1 verification resolves cross-page TU guest code")
