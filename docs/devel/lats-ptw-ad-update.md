# Page-table Accessed/Dirty updates

A page-table walker must not write an old entry back after another vCPU has
unmapped it, changed its permissions, or installed a different mapping. This
applies to intermediate entries as well as leaf PTEs. A plain low-word store of
`old_pte | accessed_dirty_bits` is not an atomic bit update: it also restores
every other low-word bit from the old snapshot.

One possible sequence is:

1. A walker reads a valid entry and retains its value in a register.
2. Another vCPU replaces the entry with a software transition entry and starts
   invalidating translations before completing page retirement.
3. The walker stores its old low word, restoring the Present bit in memory.
4. A TLB shootdown removes cached translations, but does not repair the PTE.
   The retired page can subsequently be found through the resurrected entry.

The last step can leave a page-table mapping inconsistent with the guest OS's
page-frame metadata. This is a code-level corruption mechanism; it does not by
itself identify the writer responsible for a particular crash dump.

The HAMT C walker compares the complete 64-bit entry when A/D bits need
setting. The fast soft-MMU walker uses the existing `ptw_setl` operation,
which compares the low 32 bits containing Present, Accessed and Dirty and
handles little-endian conversion. A failed comparison restarts the walk
from its root, including resetting reserved-bit constraints
which may have been derived from a large page. Successful parent updates also
update the local snapshot, so a large-page leaf does not compare against the
pre-Accessed value. If the requested bits were already set, no write is needed.

Generated HAMT refill paths carry parent Accessed bits in the existing
permission accumulator. They check the combined parent state at the leaf
boundary instead of testing and updating each parent separately. Missing
parent bits take the complete slow path. Large-page branches join before
the leaf is included in this accumulator.

A validated leaf with Accessed already set does not perform an atomic
operation. For 4K1 leaves and large-page leaves, a missing Accessed bit
branches to code after the normal return. If Dirty is already set, that
code attempts one full-width atomic
comparison against the checked snapshot. Handler generation selects
`AMCAS_DB.D` when the host advertises LAMCAS, otherwise `LL.D`/`SC.D` with
barriers. This selection adds no runtime capability check. Success updates
only Accessed and resumes the refill; a changed entry or lost reservation
takes the slow path.
There is no retry loop inside the refill exception handler. An unconditional
atomic OR would be incorrect because a now-invalid entry may use these bits
for its software encoding.

First-write handling remains with the access-type-aware C walker: an A=0,
D=0 leaf takes the slow path. For A=1,D=0 leaves, all refill modes withhold
hardware write permission until the Dirty update has occurred. Read-only
parent permissions and SMC protection can further restrict this permission.
The paired ordinary 4K leaves in 4K2 and 4K2M retain their existing
independent A/D checks and slow-path handling; they do not use this cold block.

The L4 PS check uses the original entry, and the incomplete 4K2M 1G branch
uses the complete walker. The generated sub-handler is checked against the
same size constant used by its allocator. These boundaries apply before
installing a hardware translation, independent of A/D update performance.

## Deterministic regression

Run `python3 tests/tcg/i386/latx-aot/test-ptw-ad-update.py`.

The test compiles the production C walkers and schedules a competing PTE write
after a load or immediately before the leaf A/D update. It covers normal reads
and writes, unmapping each level, replacement, permission revocation, preservation
of another writer's Dirty bit, a transition entry above 4 GiB, already-set A/D
bits, and a large-to-small mapping change during retry. TLB installation is
recorded by a stub; this is not a complete TLB-shootdown or Windows-boot test.

Use `--revision <git-ref>` to run the same scheduling cases against an earlier
source tree without modifying the checkout. Native system ROM tests remain
necessary to check the generated refill paths and their slow-path transitions.

The script requires Python 3 and a C compiler on a little-endian host. Set
`CC` to select the compiler. It reports 12 cases for each C walker. Against
an unfixed revision, the ordinary read/write cases pass while the 20 competing-update cases fail.

This test deliberately permits an already-started translation to retain its
old result before the guest completes a subsequent shootdown; it requires
that the in-memory entry is not restored. Atomic A/D updates do not replace
the separate obligations to invalidate cached translations and keep page
tables alive while readers can still use them.

`test-ptw-accessed-refill.py` in the same directory tests the production
emitter's operations with scheduled changes before CAS or LL and between LL/SC.
It checks parent Accessed fallback, leaf state preservation, lost
reservations and Dirty/write-permission handling. On LoongArch it also
encodes and executes the emitted operations as native instructions, including
concurrent replacement by a second thread. Both atomic backends are tested;
native AMCAS execution is skipped on hosts without that capability.

For the full generated handlers, build `pcid.S` with `-DPCID_WAIT` and
`-DHAMT_PTW_CONTRACT`, then run `test-ptw-contract.py` from
`tests/tcg/x86_64/system/pcid`. That ROM checks read/first-write A/D behavior,
read-only leaf and parent mappings, parent Accessed updates, reserved L4 PS
and 1G fallback in all three refill modes. It uses the existing QEMU
reserved-bit fault error encoding and checks the guest fault address.

Build the contract ROM with an x86-targeting compiler and GNU objcopy:

```sh
cd tests/tcg/x86_64/system/pcid
clang -target x86_64-none-elf -DPCID_WAIT -DHAMT_PTW_CONTRACT \
    -c pcid.S -o ptw-contract.o
objcopy -I elf64-little -O binary -j .text ptw-contract.o ptw-contract.bin
python3 test-ptw-contract.py /path/to/lats-x86_64 ptw-contract.bin
```

The runner visits 4K1, 4K2 and 4K2M sequentially without attaching a guest disk.
Use a wrapper for the executable if it needs a runtime library search path.
