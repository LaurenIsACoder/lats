#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Build a SYS guest fixture from a caller-owned licensed SPEC CPU2000 tree."""
import argparse
import hashlib
import json
import shlex
import shutil
import stat
import subprocess
from pathlib import Path

BENCHES = ('164.gzip', '175.vpr', '176.gcc', '181.mcf', '186.crafty',
           '197.parser', '252.eon', '253.perlbmk', '254.gap', '255.vortex',
           '256.bzip2', '300.twolf')

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--spec', type=Path, required=True)
    p.add_argument('--bin', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--cc', default='gcc')
    p.add_argument('--busybox', type=Path, required=True, help='Static x86_64 BusyBox with sh, echo, gzip and sha256sum')
    p.add_argument('--sizes', default='test,ref')
    a = p.parse_args()
    for key in ('spec', 'bin', 'out', 'busybox'):
        setattr(a, key, getattr(a, key).resolve())
    here = Path(__file__).resolve().parent
    a.out.mkdir(parents=True, exist_ok=True)
    tree = a.out / 'tree'
    if tree.exists():
        shutil.rmtree(tree)
    for name in ('proc', 'sys', 'dev', 'bin', 'spec', 'tmp'):
        (tree / name).mkdir(parents=True, exist_ok=True)
    for applet in ('sh', 'echo', 'busybox'):
        shutil.copy2(a.busybox, tree / 'bin' / applet)
    manifest, header = [], []
    for bench in BENCHES:
        base = a.spec / 'benchspec' / 'CINT2000' / bench
        # Read executable name from the same object used by the official harness.
        import re
        exe = re.search(r"\$exename\s*=\s*'([^']+)'", (base / 'Spec/object.pm').read_text()).group(1)
        binary = a.bin / (exe + '_base.Of.gcc830.dyn')
        shutil.copy2(binary, tree / 'bin' / exe)
        for size in a.sizes.split(','):
            directory = tree / 'spec' / bench / size
            directory.mkdir(parents=True)
            for source in (base / 'data/all/input', base / f'data/{size}/input'):
                if source.exists():
                    shutil.copytree(source, directory, dirs_exist_ok=True, symlinks=False)
            info = json.loads(subprocess.check_output(['perl', str(here / 'invoke.pl'),
                str(base / 'Spec/object.pm'), str(directory), size, '/bin/' + exe]))
            index = len(manifest)
            commands = []
            for ci, c in enumerate(info['commands']):
                args = [c['command']] + [arg for piece in c['args'] for arg in shlex.split(str(piece))]
                header.append(f'static char *const args_{index}_{ci}[] = {{' + ','.join(map(json.dumps, args)) + ',NULL};')
                commands.append('{%s,%s,%s,%s,args_%d_%d}' % (json.dumps(c['command']),
                    json.dumps(c.get('input')) if c.get('input') else 'NULL',
                    json.dumps(c['output']), json.dumps(c['error']), index, ci))
            header.append(f'static const struct command commands_{index}[] = {{' + ','.join(commands) + '};')
            output = base / f'data/{size}/output'
            expected = sorted(str(f.relative_to(output)) for f in output.rglob('*') if f.is_file())
            header.append(f'static const char *const outputs_{index}[] = {{' + ','.join(map(json.dumps, expected)) + '};')
            ref = float((base / 'data/ref/reftime').read_text().split()[1])
            manifest.append(dict(index=index, bench=bench, size=size, reference_seconds=ref,
                binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                expected=expected, **info))
    header.append('static const struct benchmark benchmarks[] = {')
    for m in manifest:
        i = m['index']
        header.append('{%s,%s,commands_%d,%d,outputs_%d,%d},' % (
            json.dumps(m['bench'] + '/' + m['size']), json.dumps('/spec/' + m['bench'] + '/' + m['size']),
            i, len(m['commands']), i, len(m['expected'])))
    header.extend(['};', f'static const size_t benchmark_count = {len(manifest)};'])
    (a.out / 'manifest.h').write_text('\n'.join(header) + '\n')
    (a.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    subprocess.run(shlex.split(a.cc) + ['-static', '-O2', '-Wall', '-Wextra', '-Werror',
        '-I' + str(a.out), str(here / 'init.c'), '-o', str(tree / 'init')], check=True)
    # Stream the archive rather than holding the complete licensed dataset in RAM.
    with (a.out / 'spec.cpio').open('wb') as archive:
        offset = 0
        def append(name, mode, payload=None, major=0, minor=0):
            nonlocal offset
            length = payload.stat().st_size if payload else 0
            encoded = name.encode() + b'\0'
            fields = [1, mode, 0, 0, 1, 0, length, 0, 0, major, minor, len(encoded), 0]
            block = b'070701' + b''.join(f'{v:08x}'.encode() for v in fields) + encoded
            archive.write(block); offset += len(block)
            padding = -offset % 4; archive.write(b'\0' * padding); offset += padding
            if payload:
                with payload.open('rb') as file: shutil.copyfileobj(file, archive)
                offset += length
            padding = -offset % 4; archive.write(b'\0' * padding); offset += padding
        append('.', stat.S_IFDIR | 0o755)
        for f in sorted(tree.rglob('*')):
            append(str(f.relative_to(tree)), stat.S_IFDIR | 0o755 if f.is_dir() else stat.S_IFREG | (0o755 if f.name == 'init' or f.parent.name == 'bin' else 0o644), None if f.is_dir() else f)
        append('dev/console', stat.S_IFCHR | 0o600, major=5, minor=1)
        append('dev/null', stat.S_IFCHR | 0o666, major=1, minor=3)
        append('TRAILER!!!', 0)
        archive.write(b'\0' * (-offset % 512))

if __name__ == '__main__':
    main()
