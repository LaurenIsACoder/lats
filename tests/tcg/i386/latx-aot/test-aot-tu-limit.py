#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check independent TU limits using the actual option parser and binary."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[4]
binary = (root / sys.argv[1]).resolve()
source = (root / 'target/i386/latx/optimization/sys-tu.c').read_text()
aot = source[source.index('static TranslationBlock *get_ts_successor('):]
assert 'MAX_TB_IN_TU' not in aot
assert aot.count('MAX_TB_IN_AOT_TU') == 4

def run(options):
    command = [str(binary), '-machine', 'none', '-display', 'none',
               '-monitor', 'none', '-serial', 'none', '-S',
               '-latx', 'select,verbose=on,aotsys=0,hamt=0,' + options]
    process = subprocess.Popen(command, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    try:
        out, _ = process.communicate(timeout=2)
        return process.returncode, out.decode(errors='replace')
    except subprocess.TimeoutExpired:
        process.terminate()
        out, _ = process.communicate(timeout=10)
        return None, out.decode(errors='replace')

for jit, aot_limit in [(1, 1024), (128, 50), (1024, 1)]:
    code, output = run(f'tumaxtb={jit},aottumaxtb={aot_limit}')
    assert code is None, output
    assert f'tu max tb = {jit}' in output, output
    assert f'aot tu max tb = {aot_limit}' in output, output
code, output = run('tumaxtb=128')
assert code is None and 'aot tu max tb = 50' in output, output
for value in [0, 1025, 2**32 + 1, 2**64 - 1]:
    code, output = run(f'aottumaxtb={value}')
    assert code not in (None, 0), output
    assert 'aottumaxtb must be between 1 and 1024' in output, output
print('PASS: independent JIT/AOT limits, defaults and invalid uint64 values')
