# Scalar floating-point comparison memory widths

COMISS/UCOMISS read 4 bytes from memory; COMISD/UCOMISD read 8 bytes.
The XMM register width must not widen the memory access to 16 bytes.
The same distinction applies to the VEX encodings.

`scalar-compare-page-boundary.c` allocates two readable pages followed by an
inaccessible page. It checks aligned, unaligned, mapped-page-crossing and
end-of-allocation operands. The 240 checks include single/double precision,
COMI/UCOMI, compare/SETcc and compare/Jcc, negative values, signed zero, equality,
and quiet NaNs. Floating-point exceptions are expected to be masked, as in a
normal process. A widened load should fault when only the scalar operand fits
before the inaccessible page.

On native x86-64 Linux, or inside an x86-64 Linux guest:

```sh
cc -O2 -Wall -Wextra scalar-compare-page-boundary.c -o scalar-compare-page-boundary
./scalar-compare-page-boundary
```

Expected output: `PASS scalar comparison boundaries: 240 cases`.

The same source supports Windows builds. For a freestanding Windows DLL,
build with `SCALAR_COMPARE_DLL` and export `scalar_compare_test`. The caller
must reserve three pages, commit the first two as readable/writable, leave the
third inaccessible, and call `scalar_compare_test(base, page_size)`. A return
value of 240 means all comparisons passed. This entry point uses no C runtime
or operating-system calls.

The decoder regression checks 32-bit and 64-bit encodings using the actual
bundled Capstone decoder. It checks architectural operand widths for
memory and register forms, with packed conversions as controls. From the repository root:

```sh
python3 tests/unit/run-latx-scalar-compare-decode.py
```

The runner builds both configurations in a temporary directory. The full
build runs 80 cases in Intel and AT&T syntax, and the diet build runs 40 cases
in Intel syntax. `--source-root` selects
another checkout for a before/after comparison. A configured LATS build also
registers `test-latx-scalar-compare-decode` in the `latx-decode` Meson test suite.
