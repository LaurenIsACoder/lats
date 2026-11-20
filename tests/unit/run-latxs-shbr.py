#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run focused SHBR tests using an existing native LATS softmmu build.

Usage: python3 tests/unit/run-latxs-shbr.py build
The test includes the production analysis and uses the bundled decoder/IR1.
Only temporary test objects are compiled; the translator build is not changed.
"""
import json
import pathlib
import shlex
import subprocess
import sys
import tempfile


def check_aot_option_activation():
    binary = build / 'lats-x86_64'
    command = [
        str(binary), '-machine', 'pc', '-m', '64', '-nographic', '-S',
        '-L', str(root / 'pc-bios'), '-latx',
        'verbose=on,aotsys=1,tumode=aot,shbr=1',
    ]
    try:
        subprocess.run(command, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, timeout=2,
                       check=False)
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ''
    else:
        raise SystemExit('SHBR AOT option check: QEMU exited too early')

    if 'WARNING: shbr requires ' in output:
        raise SystemExit('SHBR AOT option check: shbr was disabled')
    if '[LATX-SYS]       shbr      = 1' not in output:
        raise SystemExit('SHBR AOT option check: shbr level is not active')


build = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(__file__).resolve().parents[2]
entries = json.loads((build / 'compile_commands.json').read_text())
entry = next(e for e in entries if e['file'].endswith('/sys-shbr.c'))
args = shlex.split(entry['command'])
flags = []
i = 0
while i < len(args):
    if args[i] in ('-o', '-c', '-MF', '-MQ', '-MT'):
        i += 2
    elif args[i] == '-MD':
        i += 1
    else:
        flags.append(args[i])
        i += 1
flags += ['-ffunction-sections', '-fdata-sections']
with tempfile.TemporaryDirectory(prefix='latxs-shbr-test-') as tmp:
    objects = []
    for src in (root / 'tests/unit/test-latxs-shbr.c',
                root / 'target/i386/latx/ir1/ir1.c'):
        obj = str(pathlib.Path(tmp) / (src.stem + '.o'))
        subprocess.run(flags + ['-c', str(src), '-o', obj],
                       cwd=entry['directory'], check=True)
        objects.append(obj)
    decoder = build / 'target/i386/latx/libcapStone.a'
    glib = shlex.split(subprocess.check_output(
        ['pkg-config', '--libs', 'glib-2.0'], text=True))
    exe = str(pathlib.Path(tmp) / 'test-shbr')
    subprocess.run([args[0], '-no-pie', '-Wl,--gc-sections', '-o', exe] +
                   objects + [str(decoder)] + glib + ['-lpthread', '-lm'],
                   check=True)
    subprocess.run([exe], check=True)
check_aot_option_activation()
