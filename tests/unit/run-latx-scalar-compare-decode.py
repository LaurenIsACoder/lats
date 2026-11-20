#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run Capstone scalar-width checks in full and diet configurations."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root', type=Path,
                    default=Path(__file__).resolve().parents[2])
args = parser.parse_args()
source = args.source_root.resolve() / 'target/i386/latx/capStone'
test = Path(__file__).with_name('test-latx-scalar-compare-decode.c').resolve()
units = ['cs.c', 'MCInstrDesc.c', 'MCRegisterInfo.c', 'SStream.c', 'utils.c']
units += ['arch/X86/' + name + '.c' for name in (
    'X86Disassembler', 'X86DisassemblerDecoder', 'X86ATTInstPrinter',
    'X86IntelInstPrinter', 'X86Mapping', 'X86Module')]
cc = shlex.split(os.environ.get('CC', 'cc'))
failed = False
with tempfile.TemporaryDirectory(prefix='latx-scalar-decode-') as directory:
    out = Path(directory)
    (out / 'qemu').mkdir()
    # Supply ARRAY_SIZE for the standalone Capstone build.
    for diet in (False, True):
        (out / 'qemu/osdep.h').write_text(
            '#include <assert.h>\n'
            '#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))\n' +
            ('#define CONFIG_CAPSTONE_DIET 1\n' if diet else ''))
        flags = ['-std=gnu11', '-O1', '-w', '-I' + str(out),
                 '-I' + str(source / 'include'), '-DCONFIG_LATX_DEBUG=1',
                 '-DCAPSTONE_HAS_X86=1', '-DCAPSTONE_USE_SYS_DYN_MEM=1']
        if diet:
            flags += ['-DCAPSTONE_DIET=1', '-DLATX_CAPSTONE_OP_ACCESS=1']
        objects = []
        for index, unit in enumerate(units):
            obj = out / f'{index}.o'
            subprocess.run(cc + flags + ['-c', str(source / unit), '-o', str(obj)],
                           check=True)
            objects.append(str(obj))
        binary = out / 'scalar-compare-decode'
        subprocess.run(cc + flags + [str(test)] + objects + ['-o', str(binary)],
                       check=True)
        print('Capstone diet=' + str(diet), flush=True)
        failed |= subprocess.run([str(binary)]).returncode != 0
raise SystemExit(int(failed))
