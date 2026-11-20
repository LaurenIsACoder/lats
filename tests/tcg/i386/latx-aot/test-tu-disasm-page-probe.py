#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check that JIT TU cross-page probes cannot fall through to guest faults."""
from pathlib import Path
import subprocess
import sys


root = Path(__file__).resolve().parents[4]
path = "target/i386/latx/translator/sys-disasm.c"
source = (
    subprocess.check_output(["git", "show", ":" + path], cwd=root).decode()
    if "--index" in sys.argv else (root / path).read_text()
)

probe_start = source.index("static void latxs_before_inst_fetch(")
probe_end = source.index("/* Function inside X86toMIPS", probe_start)
probe = source[probe_start:probe_end]
assert "get_page_addr_code_hostp_noexit(" in probe

failure = probe[probe.index("if (phys_page2_new == -1)"):]
failure = failure[:failure.index("phys_page2_old =")]
assert "if (td->is_tu_mode)" in failure
assert "td->deadlock_avoidance = -4;" in failure
assert "td->is_tu_mode && cp_tu_pages()" not in failure

read_start = source.index("uint8_t latxs_cpu_read_code_via_qemu(")
read_end = source.index("\n}", read_start)
read_code = source[read_start:read_end]
guard = read_code.index("if (lsenv->tr_data->is_tu_mode &&")
assert "lsenv->tr_data->deadlock_avoidance" in read_code[guard:]
load = read_code.index("cpu_ldub_code(")
assert guard < load

print("PASS: JIT TU page probes fall back before guest code loads")
