#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run the merge against the actual system decoder, codegen and file writer.

Run on the LoongArch build host, with an existing matching system build.
The test replaces main and includes the merge module to exercise its internals;
all other objects, including TU translation and serialization, are unmodified.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, default=root / 'build')
args = parser.parse_args()
build = args.build.resolve()
subprocess.run(['ninja', '-C', str(build), '-j8'], check=True)
commands = json.loads((build / 'compile_commands.json').read_text())
entry = next(c for c in commands
             if c['file'].endswith('/sbt/sys-aot-tu-merge.c'))

# Read Ninja's explicit link inputs; response files may have been removed.
ninja = (build / 'build.ninja').read_text()
start = ninja.index('build lats-x86_64:')
end = ninja.index('\nbuild ', start + 1)
lines = ninja[start:end].splitlines()
link_inputs = lines[0].split(': ', 1)[1].split(' ', 1)[1].split(' | ')[0]
inputs = shlex.split(link_inputs)
link_args = next(line.split(' = ', 1)[1] for line in lines
                 if line.lstrip().startswith('LINK_ARGS = '))

with tempfile.TemporaryDirectory(prefix='aot-tu-merge-runtime-') as directory:
    work = Path(directory)
    command = shlex.split(entry['command'])
    command[command.index('-o') + 1] = str(work / 'test.o')
    command[command.index('-MF') + 1] = str(work / 'test.d')
    command[command.index('-MQ') + 1] = str(work / 'test.o')
    command[-1] = str(Path(__file__).resolve().with_suffix('.c'))
    command += ['-Werror=implicit-function-declaration',
                '-Werror=int-conversion']
    subprocess.run(command, cwd=str(build), check=True)
    main_object = 'lats-x86_64.p/softmmu_main.c.o'
    subprocess.run(['objcopy', '--redefine-sym', 'main=qemu_cli_main',
                    str(build / main_object), str(work / 'main.o')], check=True)
    inputs = [str(work / 'main.o') if name == main_object else
              str(work / 'test.o') if name.endswith(
                  '/target_i386_latx_sbt_sys-aot-tu-merge.c.o') else name
              for name in inputs]
    subprocess.run(['cc', '-o', str(work / 'test')] + inputs +
                   shlex.split(link_args.replace('$$', '$')),
                   cwd=str(build), check=True)
    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = str(build / 'subprojects/slirp')
    env['AOT_TEST_DIR'] = str(work)
    env['AOT_TEST_GROUPING'] = '1'
    subprocess.run([str(work / 'test')], env=env, check=True, timeout=30)
    env.pop('AOT_TEST_GROUPING')
    test_args = [str(work / 'test'), '-L', str(root / 'pc-bios'),
                    '-machine', 'pc', '-accel', 'tcg,thread=single',
                    '-smp', '1', '-m', '128M', '-nodefaults',
                    '-display', 'none',
                    '-serial', 'none', '-monitor', 'none', '-S', '-latx',
                    'select,verbose=on,hamt=0,aotsys=0,tumode=jit,tucp=0x70f,'
                    'tucpfast=on,tucpgen=3']
    if env.get('AOT_TEST_GDB'):
        subprocess.run(['gdb', '-batch', '-ex', 'run', '-ex',
                        'thread apply all bt', '-ex', 'info registers',
                        '-ex', 'x/12i $pc-16', '-ex',
                        'p *current_cpu->curr_exec_tb', '--args'] + test_args,
                       env=env, check=True, timeout=90)
        raise SystemExit(0)
    result = subprocess.run(test_args, env=env, timeout=90,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    print(result.stdout, end='')
    result.check_returncode()
    assert '5 TBs (deduplicated: 1)' in result.stdout
    test_args[-1] = test_args[-1].replace('tucpfast=on,tucpgen=3',
                                        'tucpfast=off,tucpgen=off')
    subprocess.run(test_args, env=env, check=True, timeout=90)
    env['AOT_TEST_JIT_EDGES'] = '1'
    subprocess.run(test_args, env=env, check=True, timeout=90)
    env.pop('AOT_TEST_JIT_EDGES')
    env['AOT_TEST_SCHEDULER'] = '1'
    subprocess.run(test_args, env=env, check=True, timeout=90)
    test_args[test_args.index('-smp') + 1] = '2'
    subprocess.run(test_args, env=env, check=True, timeout=90)
    env['AOT_TEST_PARTIAL_MERGE'] = '1'
    result = subprocess.run(test_args, env=env, timeout=45,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    print(result.stdout, end='')
    result.check_returncode()
    assert 'Failed: merge incomplete' in result.stdout
    assert 'child reap and publication' in result.stdout
    env.pop('AOT_TEST_PARTIAL_MERGE')
    env['AOT_TEST_SHUTDOWN'] = '1'
    subprocess.run(test_args, env=env, check=True, timeout=90)
    env.pop('AOT_TEST_SHUTDOWN')
    env['AOT_TEST_MAIN_SHUTDOWN'] = '1'
    subprocess.run(test_args, env=env, check=True, timeout=90)
    subprocess.run(['python3', str(Path(__file__).with_name(
        'check-cross-page-tu-format.py')), str(work / 'sys-aot-user'),
        '--require-cross-page'], env=env, check=True)
