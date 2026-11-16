# PREFETCHT0 regression

Build on x86-64 and run natively:

```sh
gcc -O2 -static prefetch-invalid-address.c -o /tmp/prefetch-invalid-address
/tmp/prefetch-invalid-address
```

Run the same binary with the LoongArch user translator:

```sh
/path/to/lats-user-x86_64 -cpu qemu64 /tmp/prefetch-invalid-address
```

For HAMT, execute the binary in a 64-bit Linux guest. No optimization option
or environment variable is needed. Both modes must print
`PREFETCH_INVALID_COUNT=6 PASSED` and exit zero. The test checks that prefetch
does not fault on inaccessible addresses; it does not measure performance.
