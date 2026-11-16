#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Generate a freestanding x86-64 ADD/SUB or AND immediate regression guest."""

import argparse
import itertools
import json
from pathlib import Path


def generate(prefix, and_only=False):
    immediates = (-2049, -2048, -2047, 2047, 2048, 2049, 0, 1, -1)
    operations = ("and",) if and_only else ("add", "sub")
    name = "AND_IMM" if and_only else "ADD_SUB_IMM"
    variants = itertools.product((16, 32, 64), ("reg", "mem"),
                                 operations, immediates)
    entry = [".section .entry,\"ax\"", ".global _start", "_start:",
             "xor %r13d,%r13d"]
    code = [".section .cases,\"ax\""]
    manifest = []
    capture_entries = []
    total = 0

    for index, (width, destination, operation, immediate) in enumerate(variants):
        suffix, register = {16: ("w", "%ax"), 32: ("l", "%eax"),
                            64: ("q", "%rax")}[width]
        operand = register if destination == "reg" else "slot(%rip)"
        code.extend([
            f".org {index * 128}", f"case{index}:",
            f"{operation}{suffix} ${immediate},{operand}",
            "pushfq", "pop %rdx", "ret",
        ])
        address = 0x700000 + index * 128
        if width == 64 and immediate in (-2048, 2048) and operation == "sub":
            capture_entries.append(address)

        mask = (1 << width) - 1
        sign = 1 << (width - 1)
        seeds = (0, 1, 0x7ff, 0x800, 0x123456789abcdead, mask,
                 mask // 2, mask // 2 + 1, 0x8000000000000000,
                 0xfffffffffffff000)
        for seed in seeds:
            left = seed & mask
            right = immediate & mask
            if operation == "add":
                result = (left + right) & mask
                carry = result < left
                overflow = bool((left ^ result) & (right ^ result) & sign)
            elif operation == "sub":
                result = (left - right) & mask
                carry = left < right
                overflow = bool((left ^ result) & (left ^ right) & sign)
            else:
                result = left & right
                carry = overflow = False
            flags = (int(carry)
                     | (int(bin(result & 255).count("1") % 2 == 0) << 2)
                     | ((left ^ right ^ result) & 16)
                     | (int(result == 0) << 6)
                     | (int(bool(result & sign)) << 7)
                     | (int(overflow) << 11))
            # AND leaves AF undefined; the other arithmetic flags are defined.
            flag_mask = 0x8c5 if and_only else 0x8d5
            flags &= flag_mask
            expected = result
            if width == 16 or (width == 32 and destination == "mem"):
                expected |= seed & ~mask
            actual = "%rax" if destination == "reg" else "slot(%rip)"
            entry.extend([
                f"movabs ${seed},%rax", "mov %rax,slot(%rip)",
                f"call case{index}", f"and ${flag_mask},%edx",
                f"cmp ${flags},%edx", "setne %r8b",
                f"movabs ${expected},%r9", f"cmp %r9,{actual}",
                "setne %r10b", "orb %r8b,%r10b", "movzbl %r10b,%r10d",
                "add %r10d,%r13d",
            ])
            total += 1
        manifest.append(dict(case=index, address=address, width=width,
                             destination=destination, op=operation, imm=immediate))

    # A Linux init guest stays alive after reporting, allowing the host runner
    # to read its result and shut down the VM through QMP.
    entry.extend([
        "test %r13d,%r13d", "jnz bad",
        "lea good(%rip),%rsi", "mov $good_len,%edx", "jmp print",
        "bad:", "mov %r13d,%eax", "lea digits(%rip),%rdi",
        "lea hex(%rip),%rsi", "mov $7,%ecx", "encode:",
        "mov %eax,%edx", "and $15,%edx", "mov (%rsi,%rdx),%dl",
        "mov %dl,(%rdi,%rcx)", "shr $4,%eax", "dec %ecx", "jns encode",
        "lea failed(%rip),%rsi", "mov $failed_len,%edx", "print:",
        "mov $1,%edi", "mov $1,%eax", "syscall",
        "hold:", "mov $34,%eax", "syscall", "jmp hold",
    ])
    data = [
        '.section .data,"aw"', 'slot: .quad 0',
        'hex: .ascii "0123456789abcdef"',
        f'good: .ascii "{name}_PASS cases={total}\\n"',
        '.set good_len,.-good', f'failed: .ascii "{name}_FAIL cases="',
        'digits: .ascii "00000000\\n"', '.set failed_len,.-failed',
        '.section .note.GNU-stack,"",@progbits',
    ]
    prefix.with_suffix(".S").write_text("\n".join(entry + code + data) + "\n")
    prefix.with_suffix(".ld").write_text("""ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x200000; .entry : { *(.entry) } :text
    . = 0x700000; .cases : { *(.cases) } :text
    . = 0x800000; .data : { *(.data) } :data
}
""")
    prefix.with_suffix(".json").write_text(json.dumps(
        dict(cases=total, variants=manifest, capture_entries=capture_entries),
        indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--and-only", action="store_true",
                        help="check AND results and defined flags instead")
    args = parser.parse_args()
    generate(args.prefix, args.and_only)
