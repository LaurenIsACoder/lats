#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the modes.S firmware on Intel and AMD CPU models with both engines."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('lat', type=Path)
parser.add_argument('tcg', type=Path)
parser.add_argument('rom', type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory() as tmp:
    for engine, binary in [('lat', args.lat), ('tcg', args.tcg)]:
        hamt = 'hamt=1,hamtmode=1,hamtconfig=0x33e9' if engine == 'lat' else 'hamt=0'
        for vendor in ['AuthenticAMD', 'GenuineIntel']:
            marker = Path(tmp) / (engine + vendor)
            result = subprocess.run([
                str(binary.resolve()), '-nodefaults', '-display', 'none', '-m', '32',
                '-no-reboot', '-accel', 'tcg,mode=' + engine, '-bios', str(args.rom),
                '-cpu', f'qemu64,vendor={vendor},+invpcid',
                '-latx', 'select,aotsys=0,tumode=off,pcid=on,' + hamt,
                '-debugcon', 'file:' + str(marker),
                '-device', 'isa-debug-exit,iobase=0xf4,iosize=4',
            ], timeout=20, capture_output=True, text=True)
            if result.returncode != (ord('S') * 2 + 1) or marker.read_bytes() != b'S':
                raise RuntimeError(f'{engine}/{vendor} failed\n{result.stderr}')
            print(f'PASS: {engine}/{vendor} real, protected16 and VM86')
