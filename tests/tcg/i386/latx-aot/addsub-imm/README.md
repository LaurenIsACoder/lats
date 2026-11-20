# ADD/SUB immediate boundaries

This Linux init guest checks 1,080 cases covering register and memory
destinations, 16/32/64-bit operands, ADD/SUB, nine immediate values around the
signed 12-bit boundary and ten initial values. It compares the complete
destination, including preserved high bits, and CF/PF/AF/ZF/SF/OF.

On a Linux machine with Python 3, Clang, LLD and cpio, run from this directory:

```sh
test_dir=$(mktemp -d)
python3 generate.py "$test_dir/guest"
clang --target=x86_64-linux-gnu -c "$test_dir/guest.S" -o "$test_dir/guest.o"
ld.lld -m elf_x86_64 -static --build-id=none -T "$test_dir/guest.ld" \
    "$test_dir/guest.o" -o "$test_dir/init"
mkdir "$test_dir/dev"
sudo mknod -m 600 "$test_dir/dev/console" c 5 1
(cd "$test_dir" && printf 'init\ndev\ndev/console\n' | cpio -o -H newc > guest.cpio)
```

On a LoongArch host, boot that archive with a supplied x86-64 Linux kernel
that supports initramfs and a serial console:

```sh
/path/to/lats-x86_64 -L /path/to/pc-bios -m 512 -smp 1 -nographic \
    -kernel /path/to/x86_64-vmlinuz -initrd "$test_dir/guest.cpio" \
    -append 'console=ttyS0 rdinit=/init norandmaps' \
    -latx select,aotsys=0,tumode=jit,tuhras=2,shbr=1,ghbr=1
```

Expect `ADD_SUB_IMM_PASS cases=1080`. The guest waits after reporting its
result. Exit the console with Ctrl-a x. A failure reports a hexadecimal count.
The unfixed hand translator fails 60 cases. For a control run, append
`,trbh64=off` to the `-latx` value. Also check TB mode with `tumode=off,tuhras=0`
and software MMU mode with `tumode=off,tuhras=0,hamt=0`.

`generate.py --and-only OUTPUT_PREFIX` instead generates 540 AND cases and
checks CF/PF/ZF/SF/OF (AF is undefined). For automated Linux guest runners,
real-mode RET/RETF tests and sanitizer checks, see
[`../instruction-boundaries/README.md`](../instruction-boundaries/README.md).
