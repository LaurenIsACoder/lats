# TU member page protection

`tu-cross-page-rewrite.c` is a standalone x86-64 reference test for writable
code on a TU member page. It executes code from the second 64 KiB region,
rewrites that region, and then verifies execution after remapping it.

`tu-rwx-member-write.c` covers the direct RWX case: guest scalar and SSE stores
rewrite a member-page instruction without calling `mprotect`, then execute the
cached code again.

Build and run it directly:

```sh
gcc -static -O2 tu-cross-page-rewrite.c -o tu-cross-page-rewrite
./tu-cross-page-rewrite

gcc -static -O2 tu-rwx-member-write.c -o tu-rwx-member-write
./tu-rwx-member-write
```

Expected output ends with:

```text
cross-page rewrite passed
cross-page remap passed
TU_RWX_MEMBER_WRITE FAILED=0
```
