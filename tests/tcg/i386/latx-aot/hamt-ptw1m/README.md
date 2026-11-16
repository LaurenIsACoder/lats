# HAMT 1M SMC regression

Run the host-side test from the repository root:

```sh
python3 tests/tcg/i386/latx-aot/test-hamt-ptwdata1m.py
```

It extracts and compiles the production allocation and SMC counter functions,
tests the 255/256 boundary, repeated operations, adjacent regions and
reallocation, and checks the refill emitter source for scaled halfword loads.
The source check does not execute the emitted instructions. `--ptw-source` and
`--refill-source` accept alternate source files for negative controls.

The guest test executes and modifies 256 x86 code pages in a physically
contiguous, 1M-aligned region. It verifies a 255-page control and three 256-page
execution rounds. This exercises LATS but does not directly observe the HAMT
counter. Compare against an unfixed binary as a negative control.

Build the guest on x86-64 Linux with a static C library:

```sh
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -static \
    -o hamt-ptw1m-guest tests/tcg/i386/latx-aot/hamt-ptw1m/guest.c
```

Transfer the resulting executable to the LATS host. It is not checked in.
Run with an x86-64 Linux kernel supporting initramfs, proc, pagemap and
transparent huge pages, and an existing LATS build and BIOS directory:

```sh
python3 tests/tcg/i386/latx-aot/hamt-ptw1m/run-linux.py \
    --binary /path/to/build/lats-x86_64 --bios /path/to/pc-bios \
    --kernel /path/to/x86-64-kernel --guest /path/to/hamt-ptw1m-guest \
    --output /path/to/new-output-directory
```

The output directory must not already exist. The runner creates an initramfs,
records the command and VM log, and writes `result.json` only on success.
It uses no disk image and needs no host root privileges. The guest must run as
PID 1 in the isolated VM because it powers the VM off when finished. Failure
to obtain a physically contiguous aligned region is reported as a test failure,
not a regression result. The default timeout is 180 seconds; `--timeout` and
`--latx` can override the runner defaults.
