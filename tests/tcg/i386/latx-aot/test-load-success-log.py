#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Ensure opening attempts cannot be mistaken for successful AOT loads."""
from pathlib import Path

root = Path(__file__).resolve().parents[4]
loader = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
start = loader.index("AOTPublishedFile *load_aot_file(")
load = loader[start:loader.index("\n}", start) + 2]
assert "Opening:" not in load and "opening " not in load
success = load.index('"[AOT][Load] Opened:')
assert load.index("published->generation =") < success < load.index("fail:")
assert load.index("aot_load_file_validate_index(") < success
assert load.index("aot_load_file_prepare_hash(") < success
print("PASS: AOT load success is logged only after validation and setup")
