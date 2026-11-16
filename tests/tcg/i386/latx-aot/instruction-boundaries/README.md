# Instruction immediate boundaries

These tests cover three additional boundary defects alongside the existing
`../addsub-imm/` SUB regression:

* Near RET with a 16-bit stack must check the complete stack delta before
  emitting ADDI. For a 16-bit return, `ret 2046` adds 2048 bytes, outside
  the signed 12-bit immediate range.
* Real-mode/VM86 far RET with a 16-bit stack must write its register-add
  fallback into the temporary subsequently stored to SP. `retf 2044`
  crosses this boundary for a 16-bit return.
* AND's mask classifier must not call `ctz` or `clz` on zero. AND leaves
  AF undefined; tests compare only CF/PF/ZF/SF/OF.

## Host emitter tests

Run with Python 3 and a C compiler supporting UndefinedBehaviorSanitizer:

```sh
python3 test-emission.py
```

The script extracts the actual production functions, compiles them with a
checked register model at `-O0` and `-O2`, and uses the project's `-fwrapv`
semantics. It rejects out-of-range ADDI operands and reads of undefined
temporaries. It checks:

* All 65,536 unsigned immediate values, several initial stack values and
  return widths, 16/32/64-bit near stacks and 16/32-bit far stacks.
* Negative stack updates and preservation of bits above SP.
* AND mask equivalence for all signed 16-bit values, the 32-bit mask
  boundaries and 10,000 deterministic pseudorandom values, under UBSan.

These are emitter tests. They do not boot a guest or exercise memory faults,
protected-mode far-return checks, or VM86 mode transitions.

## Real-mode execution

On a LoongArch Linux host with Python 3, Clang and LLD:

```sh
python3 run-boot.py --binary /path/to/lats-x86_64 --bios /path/to/pc-bios \
    --output /tmp/instruction-boundaries-soft --trace
```

The runner builds a private boot disk and starts a VM. No guest kernel,
root privileges or existing disk image is required. It checks 132 cases:
16/32-bit near and far returns on a 16-bit stack, values on both sides of
the ADDI boundary, unsigned immediate limits, SP wraparound, preserved
upper ESP bits, and 8/16/32-bit AND results and defined flags. It reports
`INSTRUCTION_BOUNDARIES_PASS all cases=132`; the debug-exit device returns
33 on success. `--group near`, `--group far` and `--group and` isolate the
36, 36 and 60 cases respectively. `--trace` saves generated-code logs.

Use a new output directory for each run. The default uses software MMU.
Also run with each of these `--latx` values:

```text
select,aotsys=0,tumode=jit,tuhras=2,shbr=1,ghbr=1
select,aotsys=0,tumode=off,tuhras=0,shbr=1,ghbr=1
```

The guest remains in real mode, so these runs do not establish protected-mode
or VM86 coverage, or imply that every configured optimization is used.

## Linux x86-64 execution

Supply an x86-64 Linux kernel supporting initramfs and a serial console:

```sh
python3 run-linux.py --binary /path/to/lats-x86_64 --bios /path/to/pc-bios \
    --kernel /path/to/vmlinuz --output /tmp/addsub-imm-jit
python3 run-linux.py --binary /path/to/lats-x86_64 --bios /path/to/pc-bios \
    --kernel /path/to/vmlinuz --output /tmp/and-imm-jit --and-only
```

The first runs the original 1,080 ADD/SUB cases; the second runs 540 AND
cases, including zero, for 16/32/64-bit register and memory operands.
Complete destinations, preserved high bits and defined flags are checked.
The runner constructs the initramfs directly and stops its VM after reading
the exact pass marker. It records the command and binary/kernel/guest hashes.
Repeat with `--latx` set to the TB configuration above and to
`select,aotsys=0,tumode=off,tuhras=0,hamt=0,shbr=1,ghbr=1`.

AND may pass its guest tests even before the fix: the regression that detects
its zero-input undefined behavior is the host UBSan test. Guest success must
not be described as a reproduction of an AND miscalculation.
