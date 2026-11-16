#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run the ADD/SUB or AND Linux init guest without requiring root or cpio."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time


def member(name, data, mode, inode, major=0, minor=0):
    name = name.encode() + b'\0'
    fields = [inode, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(name), 0]
    result = b'070701' + b''.join(f'{x:08x}'.encode() for x in fields) + name
    result += bytes(-len(result) % 4)
    return result + data + bytes(-len(data) % 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'bios', 'kernel', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--and-only', action='store_true')
    parser.add_argument('--latx', default='select,aotsys=0,tumode=jit,tuhras=2,shbr=1,ghbr=1')
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    generator = Path(__file__).resolve().parent.parent / 'addsub-imm/generate.py'
    subprocess.run([sys.executable, str(generator), str(output / 'guest'),
                    *(['--and-only'] if args.and_only else [])], check=True)
    subprocess.run(['clang', '--target=x86_64-linux-gnu', '-c',
                    str(output / 'guest.S'), '-o', str(output / 'guest.o')], check=True)
    subprocess.run(['ld.lld', '-m', 'elf_x86_64', '-static', '--build-id=none',
                    '-T', str(output / 'guest.ld'), str(output / 'guest.o'),
                    '-o', str(output / 'init')], check=True)
    init = (output / 'init').read_bytes()
    archive = member('init', init, 0o100755, 1)
    archive += member('dev', b'', 0o40755, 2)
    archive += member('dev/console', b'', 0o20600, 3, 5, 1)
    archive += member('TRAILER!!!', b'', 0, 4)
    (output / 'guest.cpio').write_bytes(archive)
    command = [str(args.binary.resolve()), '-L', str(args.bios.resolve()),
               '-m', '512', '-smp', '1', '-nographic', '-monitor', 'none',
               '-kernel', str(args.kernel.resolve()), '-initrd', str(output / 'guest.cpio'),
               '-append', 'console=ttyS0 rdinit=/init norandmaps clocksource=tsc tsc=reliable',
               '-latx', args.latx]
    (output / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    cases = json.loads((output / 'guest.json').read_text())['cases']
    name = 'AND_IMM' if args.and_only else 'ADD_SUB_IMM'
    expected = f'{name}_PASS cases={cases}'
    log_path = output / 'vm.log'
    with log_path.open('w') as log:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + args.timeout
            while True:
                lines = log_path.read_text(errors='replace').splitlines()
                if expected in lines:
                    break
                if (process.poll() is not None or time.monotonic() >= deadline
                        or any(name + '_FAIL' in line or 'Kernel panic' in line
                               for line in lines)):
                    raise RuntimeError(f'Guest did not pass; see {log_path}')
                time.sleep(0.1)
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    identity = {key + '_sha256': hashlib.sha256(path.read_bytes()).hexdigest()
                for key, path in [('binary', args.binary), ('kernel', args.kernel),
                                  ('guest', output / 'init')]}
    identity.update(result=expected, latx=args.latx)
    (output / 'result.json').write_text(json.dumps(identity, indent=2) + '\n')
    print(expected)


if __name__ == '__main__':
    main()
