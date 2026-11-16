#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Compile the production emitters with a checked integer-register model."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[4]


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


misc = (ROOT / "target/i386/latx/translator/sys-tr-misc.c").read_text()
logic = (ROOT / "target/i386/latx/translator/sys-tr-logic.c").read_text()
production = "\n".join([
    function(misc, "void latxs_update_stack_pointer("),
    function(misc, "static void latxs_do_translate_ret_far_real("),
    function(logic, "static inline bool imm_mask("),
])
source = (HERE / "emission-harness.c").read_text().replace(
    "/* PRODUCTION_FUNCTIONS */", production)
with tempfile.TemporaryDirectory(prefix="lats-instruction-boundaries-") as tmp:
    tmp = Path(tmp)
    (tmp / "test.c").write_text(source)
    for optimization in ("-O0", "-O2"):
        subprocess.run([
            *shlex.split(os.environ.get("CC", "cc")), "-std=gnu11",
            optimization, "-fwrapv", "-fsanitize=undefined",
            "-fno-sanitize-recover=all", str(tmp / "test.c"),
            "-o", str(tmp / "test"),
        ], check=True)
        # Separate processes preserve independent failure signals on old code.
        results = [subprocess.run([str(tmp / "test"), group]).returncode
                   for group in ("near", "far", "mask")]
        if any(results):
            raise SystemExit(f"{optimization}: near/far/mask statuses: {results}")
