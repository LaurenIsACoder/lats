# Dense code pages and writable aliases

```sh
cc -O2 -Wall -Wextra -o smc-protect-bench tests/latx/smc-protect-bench.c
./smc-protect-bench 128 3
./smc-protect-bench 128 256  # longer warm control
python3 tests/latx/test-smc-protect.py
```

The benchmark is an x86-64 program, runnable natively, with a user translator,
or in a system guest through an existing runner. It does not manage runners.
The two optional arguments are the number of pages and execution passes.

Each page contains independent 16-byte functions returning different constants.
Pass 0 measures translation plus execution; subsequent passes provide a warm
control. Every return value is checked. Afterward the test creates a new writable
alias of the already translated file pages, reads the alias, changes one function
per page and checks that executing the original mapping returns the new value.
It also checks an alias filled with writable translations before code execution.
The file is unlinked immediately and mappings are private to the test process.

Compare pass 0 with identical page count, guest and translator settings. Use
alternating runs on the same host CPU, and report a score of
`100 * baseline_pass0_ns / candidate_pass0_ns`. This measures dense-page
translation cost, not total Java performance. Also check Java startup separately.
The program requests a fixed code address as an mmap hint, without MAP_FIXED,
and prints CODE_ADDRESS. Check that this address matches between performance
runs: random code addresses substantially change this tiny-function workload's
warm cost. Aliases still receive addresses selected by the OS. For warm timing,
use 256 passes and compare passes 64 onward; two short warm passes are insufficient.
Run correctness checks with HAMT enabled and disabled, and with multiple vCPUs.
Parallel vCPU execution deliberately retains the original protection scan.

The Python regression compiles the production `tlb_protect_code`,
`tlb_set_page_full` and `tlb_set_page_full_fast` with a small MMU fixture.
Memory-region lookup, locking and final TLB address comparison are replaced;
scan decisions and write-flag construction are the real production code.
It checks repeated protection, unprotect/reprotect, single-vCPU MTTCG,
serialized and parallel multiple vCPUs, stub behavior, read-only pages and
QEMU's dirty bitmap. The constructors are tested with 4 KiB/2 MiB mappings
and protection that completes just before their TLB lock is acquired.
`CC` can select a compiler or compiler wrapper.
