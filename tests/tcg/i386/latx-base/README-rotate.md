# rotate-imm-dead-flags

This standalone x86-64 test checks immediate ROL/ROR results when the
instruction's flags are overwritten and therefore dead.

Build and run the native reference directly:

```sh
gcc -static -O2 rotate-imm-dead-flags.c -o rotate-imm-dead-flags
./rotate-imm-dead-flags
```

Expected output:

```text
ROTATE_COUNT=32768 FAILED=0
```
