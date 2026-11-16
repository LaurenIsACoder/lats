#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Build and run the real-mode guest on a supplied LATS system binary."""

import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--bios', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--group', choices=('near', 'far', 'and', 'all'), default='all')
    parser.add_argument('--latx', default='select,aotsys=0,tumode=off,tuhras=0,hamt=0')
    parser.add_argument('--timeout', type=int, default=60)
    parser.add_argument('--trace', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    subprocess.run([sys.executable, str(Path(__file__).with_name('build-boot.py')),
                    str(output), '--group', args.group], check=True)
    command = [str(args.binary.resolve()), '-L', str(args.bios.resolve()),
               '-m', '64', '-display', 'none', '-serial', 'none',
               '-monitor', 'none', '-debugcon', 'file:' + str(output / 'result.txt'),
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
               '-drive', 'file=' + str(output / 'boot.img') + ',format=raw',
               '-no-reboot', '-latx', args.latx + (',dump=01110' if args.trace else '')]
    if args.trace:
        command += ['-dfilter', '0x8000..0xffff']
    (output / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (output / 'vm.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                timeout=args.timeout)
    text = (output / 'result.txt').read_text()
    manifest = json.loads((output / 'manifest.json').read_text())
    expected = f'INSTRUCTION_BOUNDARIES_PASS {args.group} cases={manifest["cases"]}\n'
    if result.returncode != 33 or text != expected:
        raise SystemExit(f'FAIL: exit={result.returncode}, guest={text!r}; see {output}')
    print(text, end='')


if __name__ == '__main__':
    main()
