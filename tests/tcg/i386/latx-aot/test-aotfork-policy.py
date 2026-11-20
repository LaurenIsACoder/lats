#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Check that aotfork only gates runtime flush saves."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[4]
options = (root / 'target/i386/latx/latx-options.c').read_text()
header = (root / 'target/i386/latx/include/latx-options.h').read_text()
save = (root / 'target/i386/latx/sbt/sys-aot-save.c').read_text()


def function(signature):
    start = save.index(signature + '\n{')
    pos = save.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (save[end] == '{') - (save[end] == '}')
        end += 1
    return save[start:end]


errors = []
if 'int option_aot_fork = 0;' not in options:
    errors.append('aotfork is not disabled by default')
if 'extern int option_aot_fork;' not in header:
    errors.append('aotfork option declaration is missing')
if not re.search(r'\.name = "aotfork",\s*\.type = QEMU_OPT_BOOL,',
                 options):
    errors.append('aotfork is not a boolean latx option')
if 'parse_options_bool("aotfork", option_aot_fork);' not in options:
    errors.append('aotfork is not parsed')

flush = function('void aot_save_on_tb_flush(int mtcc_id)')
guard = re.search(r'if \(!option_aot_fork\) \{\s*return;\s*\}', flush)
if not guard:
    errors.append('runtime flush save is not gated by aotfork')
else:
    fork = flush.find('aot_start_save_child(&child_job)')
    if fork < 0 or fork < guard.end():
        errors.append('runtime flush can fork before the aotfork check')

shutdown = function('void aot_save_on_shutdown(void)')
if 'option_aot_fork' in shutdown:
    errors.append('aotfork incorrectly gates final shutdown saves')
if 'aot_start_save_child(&job)' not in shutdown:
    errors.append('final shutdown save is not started')
if 'option_wait_aot' not in shutdown:
    errors.append('waitaot no longer controls final save completion')
failure = shutdown.find('else if (pid < 0)')
wait = shutdown.find('aot_wait_save_child();')
fallback = shutdown.find('aot_save_run_sync(-1);')
if failure < 0 or wait < failure or fallback < wait:
    errors.append('failed shutdown forks do not wait and save synchronously')
if 'strerror(fork_errno)' not in shutdown:
    errors.append('fork failure fallback does not report the fork error')

if errors:
    print('\n'.join(f'FAIL: {error}' for error in errors))
    sys.exit(1)

print('PASS: aotfork gates flushes; shutdown forks fall back to synchronous save')
