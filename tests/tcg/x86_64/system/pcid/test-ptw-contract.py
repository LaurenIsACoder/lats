#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the HAMT_PTW_CONTRACT ROM against each generated refill mode."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile

from run import request, wait_for_marker

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('rom', type=Path)
parser.add_argument('--config', choices=['3369', '33a9', '33e9'],
                    action='append', help='default: all three refill modes')
parser.add_argument('--timeout', type=float, default=60)
args = parser.parse_args()

for config in args.config or ['3369', '33a9', '33e9']:
    with tempfile.TemporaryDirectory() as temp:
        work = Path(temp)
        marker, qmp = work / 'result', work / 'qmp'
        latx = ('select,aotsys=0,tumode=off,pcid=off,pcidasid=off,'
                'pcidcache=off,hamt=1,hamtmode=1,hamtconfig=0x' + config)
        command = [str(args.binary.resolve()), '-machine', 'pc',
                   '-accel', 'tcg,mode=lat', '-nodefaults', '-display', 'none',
                   '-smp', '1', '-m', '32', '-cpu', 'qemu64,+pdpe1gb',
                   '-bios', str(args.rom.resolve()), '-no-reboot',
                   '-debugcon', 'file:' + str(marker),
                   '-qmp', f'unix:{qmp},server=on,wait=off', '-latx', latx]
        with (work / 'stderr').open('w+') as log:
            proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            try:
                wait_for_marker(proc, marker, 1, args.timeout)
                with socket.socket(socket.AF_UNIX) as sock:
                    sock.settimeout(10)
                    sock.connect(str(qmp))
                    with sock.makefile('rwb', buffering=0) as stream:
                        request(stream, 'qmp_capabilities')
                        request(stream, 'stop')
                        if marker.read_bytes() != b'\x55':
                            state = request(stream, 'human-monitor-command',
                                            {'command-line': 'info registers -a'})
                            raise RuntimeError('PTW contract failed:\n' + state)
                print(json.dumps({'config': config, 'passed': True}), flush=True)
            except Exception:
                log.seek(0)
                print(log.read())
                raise
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
