# Repeated destination invalidation

```sh
cc -O2 -Wall -Wextra -o tb-relink-bench tests/latx/tb-relink-bench.c
./tb-relink-bench 64 2000000
./tb-relink-bench 64 2000000 --alternate-targets
python3 tests/latx/test-tb-relink.py
```

The benchmark is an x86-64 program. Run it natively, with a user translator,
or in a system guest using your existing runner. Static linking is useful for
small guest images. It does not start or configure a translator.

One generated page contains a direct jump to a second page. Every round
changes the destination's immediate value, leaving the source unchanged.
Each call must return the new value. The output includes elapsed nanoseconds,
checksum and ns/call for every round. Compare the full curve, especially rounds
16 onward, with identical arguments and translator settings. A lifetime limit
of 15 links leaves the source permanently unlinked once destinations have been
invalidated enough times. Relinking immediately avoids both that lifetime
limit and an arbitrary retry delay.

`--alternate-targets` keeps two unchanged file-backed target pages mapped and
alternates them at the executable destination address. The source stays
unchanged and both old targets remain available. This exercises the mapping
changes that can make cross-page checks fail repeatedly, without rewriting
the target code. Each call must return the value of the currently mapped page.
Also try `256 1 --alternate-targets` to measure the first-call cost when a
mapping changes after every call; the mapping operation itself is not timed.

Use alternating baseline/candidate runs on the same CPU. A relative score is
`100 * baseline_ns_per_call / candidate_ns_per_call`; report the round range
used. Neither the absolute call cost nor this score represents all Java code.

The Python regression compiles the actual `tb_add_jump` and `unlink_old_dest`.
Code patching and target lookup use a small fixture, with pthread locks. It
checks 10,000 unlinks on each of two exits concurrently, another 10,000 switches
between still-valid targets, immediate recovery even with old saturated count
bits, occupied/tagged slots, flag preservation and rejected targets. It does not
execute generated host instructions; the benchmark supplies that integration
check. `CC` can select a compiler, including a compiler wrapper.
