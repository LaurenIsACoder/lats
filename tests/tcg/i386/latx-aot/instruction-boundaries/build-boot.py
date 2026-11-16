#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Build a BIOS-bootable real-mode RET/RETF and AND boundary regression."""

import argparse
import json
from pathlib import Path
import subprocess


def generate(group):
    entry = [
        '.section .text,"ax"', '.code16', '.global _start', '_start:',
        'ljmp $0,$boot', 'boot:', 'cli', 'xor %ax,%ax',
        'mov %ax,%ds', 'mov %ax,%es', 'mov %ax,%ss', 'mov $0x7000,%sp',
        'cld', 'mov $disk_packet,%si', 'mov $0x42,%ah', 'int $0x13',
        'jc disk_error', 'ljmp $0,$stage2',
        'disk_error:', 'mov $0xe9,%dx', 'mov $69,%al', 'out %al,%dx',
        'jmp disk_error', '.balign 4', 'disk_packet:',
        '.byte 16,0', '.word 63', '.word 0x8000,0', '.quad 1',
        '.org 510', '.word 0xaa55', '.org 1024', 'stage2:',
        'cli', 'xor %ax,%ax', 'mov %ax,%ds', 'mov %ax,%es',
        'mov %ax,%ss', 'cld', 'xor %edi,%edi',
    ]
    # Stage 2 is linked at 0x8000 but stored at disk sector 2.
    routines = []
    cases = 0
    if group in ('near', 'far', 'all'):
        kinds = ('near', 'far') if group == 'all' else (group,)
        for kind in kinds:
            for width in (16, 32):
                suffix = 'w' if width == 16 else 'l'
                frame = width // 8 * (2 if kind == 'far' else 1)
                immediates = sorted({0, 1, 2047 - frame, 2048 - frame,
                                     2049 - frame, 4096, 32767, 32768, 65535})
                for immediate in immediates:
                    label = f'{kind}_{width}_{immediate}'
                    operation = 'ret' if kind == 'near' else 'lret'
                    routines += [label + ':', f'{operation}{suffix} ${immediate}']
                    for seed in (0x12344000, 0xabcdfff0):
                        expected = (seed & ~65535) | ((seed + immediate) & 65535)
                        call = (f'call{suffix} {label}' if kind == 'near'
                                else f'lcall{suffix} $0,${label}')
                        entry += [f'mov ${seed},%esp', call,
                                  f'cmp ${expected},%esp', 'je 1f',
                                  'inc %edi', '1:']
                        cases += 1
    if group in ('and', 'all'):
        for width, operand, suffix in ((8, '%al', 'b'), (8, '%ah', 'b'),
                                       (16, '%ax', 'w'), (32, '%eax', 'l')):
            mask = (1 << width) - 1
            for immediate in sorted({0, 1, mask, mask >> 1, 1 << (width - 1)}):
                for seed in (0, 0x12345678, 0xffffffff):
                    start = 8 if operand == '%ah' else 0
                    value = (seed >> start) & mask & immediate
                    expected = (seed & ~(mask << start)) | (value << start)
                    flags = (int(bin(value & 255).count('1') % 2 == 0) << 2
                             | int(value == 0) << 6
                             | int(bool(value & (1 << (width - 1)))) << 7)
                    entry += ['mov $0x4000,%esp', f'mov ${seed},%eax',
                              f'and{suffix} ${immediate},{operand}',
                              'pushfw', 'pop %dx', 'and $0x8c5,%dx',
                              f'cmp ${flags},%dx', 'jne 2f',
                              f'cmp ${expected},%eax', 'je 1f',
                              '2:', 'inc %edi', '1:']
                    cases += 1
    entry += [
        'mov $0x4000,%esp', 'test %edi,%edi', 'jnz failed',
        'mov $pass_text,%si', 'mov $0x10,%bx', 'jmp print',
        'failed:', 'mov $fail_text,%si', 'mov $0x11,%bx',
        'print:', 'lodsb', 'test %al,%al', 'jz exit',
        'mov $0xe9,%dx', 'out %al,%dx', 'jmp print',
        'exit:', 'mov %bx,%ax', 'mov $0xf4,%dx', 'out %al,%dx',
        'halt:', 'hlt', 'jmp halt',
        f'pass_text: .asciz "INSTRUCTION_BOUNDARIES_PASS {group} cases={cases}\\n"',
        f'fail_text: .asciz "INSTRUCTION_BOUNDARIES_FAIL {group}\\n"',
    ]
    return '\n'.join(entry + routines) + '\n', cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--group', choices=('near', 'far', 'and', 'all'), default='all')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    assembly, cases = generate(args.group)
    src = args.output / 'boot.S'
    src.write_text(assembly)
    obj = args.output / 'boot.o'
    raw = args.output / 'boot.bin'
    subprocess.run(['clang', '--target=i386-linux-gnu', '-c', str(src),
                    '-o', str(obj)], check=True)
    subprocess.run(['ld.lld', '-m', 'elf_i386', '-Ttext=0x7c00',
                    '--oformat=binary', str(obj), '-o', str(raw)], check=True)
    data = raw.read_bytes()
    assert len(data) - 1024 <= 63 * 512
    image = data[:512] + data[1024:]
    image += bytes(1024 * 1024 - len(image))
    (args.output / 'boot.img').write_bytes(image)
    (args.output / 'manifest.json').write_text(json.dumps(
        {'group': args.group, 'cases': cases, 'stage2': '0x8000'}, indent=2) + '\n')
    print(f'Built {args.output / "boot.img"}: {cases} {args.group} cases')


if __name__ == '__main__':
    main()
