#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run production GHBR analysis tests with a native system-mode build."""
import json
import pathlib
import shlex
import subprocess
import sys
import tempfile

build = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(__file__).resolve().parents[2]
entries = json.loads((build / 'compile_commands.json').read_text())
entry = next(e for e in entries if e['file'].endswith('/sys-ghbr.c'))
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
with tempfile.TemporaryDirectory(prefix='latxs-ghbr-') as tmp:
    objects = []
    for src in (root / 'tests/unit/test-latxs-ghbr.c',
                root / 'target/i386/latx/translator/sys-opnd-process.c',
                root / 'target/i386/latx/ir1/ir1.c'):
        obj = str(pathlib.Path(tmp) / (src.stem + '.o'))
        subprocess.run(flags + ['-c', str(src), '-o', obj],
                       cwd=entry['directory'], check=True)
        objects.append(obj)
    glib = shlex.split(subprocess.check_output(
        ['pkg-config', '--libs', 'glib-2.0'], text=True))
    exe = str(pathlib.Path(tmp) / 'test-ghbr')
    subprocess.run([args[0], '-no-pie', '-Wl,--gc-sections', '-o', exe] +
                   objects + [str(build / 'target/i386/latx/libcapStone.a')] +
                   glib + ['-lpthread', '-lm'], check=True)
    subprocess.run([exe], check=True)

activation_checks = (
    ('tumode=jit,ghbr=on', 1),
    ('tumode=jit,ghbr=1', 1),
    ('tumode=jit,ghbr=2', 2),
    ('aotsys=1,tumode=aot,ghbr=on', 1),
    ('tumode=jit,ghbr=off', 0),
    ('tumode=jit,ghbr=0', 0),
    ('tumode=jit', 0),
)
for options, expected in activation_checks:
    command = [str(build / 'lats-x86_64'), '-machine', 'pc', '-m', '64',
               '-nographic', '-S', '-L', str(root / 'pc-bios'),
               '-latx', 'verbose=on,' + options]
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=2)
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or b'').decode(errors='replace')
    else:
        raise SystemExit(f'QEMU exited early: {result.stdout!r}')
    if f'[LATX-SYS]       ghbr      = {expected}' not in output:
        raise SystemExit(f'GHBR option activation failed: {options}\n{output}')
    warning = 'ghbr=2 ignores memory liveness barriers'
    if expected == 2 and warning not in output:
        raise SystemExit(f'GHBR level-2 warning missing:\n{output}')
print(f'GHBR option activation checks: {len(activation_checks)} passed')

invalid = [str(build / 'lats-x86_64'), '-machine', 'pc', '-m', '64',
           '-nographic', '-S', '-L', str(root / 'pc-bios'),
           '-latx', 'tumode=jit,ghbr=3']
result = subprocess.run(invalid, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, timeout=2)
output = result.stdout.decode(errors='replace')
if result.returncode == 0 or 'invalid ghbr level: 3' not in output:
    raise SystemExit(f'Invalid GHBR level was not rejected:\n{output}')
print('GHBR invalid-level check: passed')
