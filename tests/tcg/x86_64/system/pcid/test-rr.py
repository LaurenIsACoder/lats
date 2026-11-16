#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""A build configured with --enable-latx-rr must reject shared HAMT ASIDs."""
from pathlib import Path
import subprocess
import sys

binary = Path(sys.argv[1]).resolve()
result = subprocess.run([
    str(binary), '-S', '-smp', '2', '-nodefaults', '-display', 'none',
    '-accel', 'tcg,mode=lat,thread=single',
    '-latx', 'select,pcid=on,pcidasid=on,hamt=1,hamtmode=1,hamtconfig=0x33e9',
], capture_output=True, text=True, timeout=10)
if result.returncode == 0 or 'pcidasid requires multi-threaded TCG' not in result.stderr:
    raise SystemExit(f'Expected single-thread rejection, got {result.returncode}\n'
                     + result.stderr)
print('PASS: single-threaded HAMT ASID retention rejected')
