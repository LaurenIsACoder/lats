#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise the real serialized atomic implementation with faulting RAM probes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-atomic.c").read_text()
body = source[source.index("void latx_helper_aot_atomic_request("):
              source.index("static uint64_t atomic_access(")]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifdef __x86_64__
#include <sys/mman.h>
#endif
#include "lats/aot-atomic.h"
#include "latx-native-atomic.h"
#define CC_C 1
#define CC_P 4
#define CC_A 16
#define CC_Z 64
#define CC_S 128
#define CC_O 2048
#define CC_OP_EFLAGS 1
#define R_EAX 0
#define R_EDX 2
#define TARGET_PAGE_SIZE 4096
#define TARGET_PAGE_MASK (~UINT64_C(4095))
#define EXCP0D_GPF 13
#define EXCP_ATOMIC 256
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define g_assert assert
#define g_assert_not_reached() abort()
#define error_report(...) fprintf(stderr, __VA_ARGS__)
typedef struct CPUArchState {
    uint64_t regs[16], eflags, eip, cc_src;
    unsigned cc_op;
    bool native_recovered;
    unsigned is_fcsr_simd;
    LatxNativeAtomicRequest aot_native_atomic;
} CPUX86State;
typedef struct CPUState { int exception_index; } CPUState;
static CPUState cpu;
static bool exclusive = true;
static jmp_buf fault;
/* Deliberately noncontiguous host pages. */
static uint8_t ram[2][8192];
static bool readable[2], writable[2];
typedef uint64_t ram_addr_t;
static unsigned invalidations;
static ram_addr_t qemu_ram_addr_from_host_nofail(void *p)
{ return (uint8_t *)p - (uint8_t *)ram; }
static void tb_invalidate_phys_range(ram_addr_t first, ram_addr_t last)
{
    assert(last >= first && last - first < 8);
    assert((first & 8191) < 4096 && (last & 8191) < 4096);
    invalidations++;
}
static CPUState *env_cpu(CPUX86State *env) { (void)env; return &cpu; }
static bool cpu_in_exclusive_context(CPUState *c) { (void)c; return exclusive; }
static void cpu_loop_exit(CPUState *c) { longjmp(fault, c->exception_index); }
static void raise_exception_err_ra(CPUX86State *env, int exception, int error, uintptr_t ra)
{ (void)env; (void)error; (void)ra; longjmp(fault, exception); }
static int cpu_mmu_index(CPUX86State *env, bool fetch) { (void)env; (void)fetch; return 1; }
static void *probe(uint64_t address, int bytes, bool write)
{
    assert(address >= 0x1000 && address < 0x3000);
    unsigned page = (address - 0x1000) / 4096, offset = address & 4095;
    assert(offset + bytes <= 4096);
    if (!(write ? writable[page] : readable[page])) longjmp(fault, 14);
    return ram[page] + offset;
}
static void *probe_write(CPUX86State *env, uint64_t a, int n, int mmu, uintptr_t ra)
{ (void)env; (void)mmu; (void)ra; return probe(a, n, true); }
static void *probe_read(CPUX86State *env, uint64_t a, int n, int mmu, uintptr_t ra)
{ (void)env; (void)mmu; (void)ra; return probe(a, n, false); }
''' + body + r'''
static CPUX86State env;
static uint32_t descriptor(unsigned operation, unsigned log_bytes)
{ return operation | log_bytes << 4 | 2 << 6 | 5 << 11; }
static void reset(void)
{
    memset(&env, 0, sizeof(env)); memset(ram, 0, sizeof(ram));
    invalidations = 0;
    readable[0] = readable[1] = writable[0] = writable[1] = true;
    env.eflags = 0x602; env.eip = 0x400000;
}
#ifdef __x86_64__
static void compare_native(void)
{
    uint8_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(code != MAP_FAILED);
    /* rdi: memory; rsi: accumulator, source register, input/output flags. */
    const uint8_t prefix[] = {0x48,0x8b,0x06, 0x48,0x8b,0x4e,0x08,
                              0xff,0x76,0x10, 0x9d};
    const uint8_t suffix[] = {0x9c,0x5a, 0x48,0x89,0x06, 0x48,0x89,0x4e,0x08,
                              0x48,0x89,0x56,0x10, 0xc3};
    const uint64_t values[] = {0, 1, 7, 15, 127, 128, 255, 256,
        UINT64_C(0x7fffffff), UINT64_C(0x80000000),
        UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000), UINT64_MAX};
    unsigned cases = 0;
    for (unsigned op = LATS_ATOMIC_ADD; op <= LATS_ATOMIC_NEG; op++) {
        for (unsigned log = 0; log < 4; log++) {
            unsigned at = sizeof(prefix), bits = (1U << log) * 8;
            memcpy(code, prefix, at);
            if (log == 1) code[at++] = 0x66;
            if (log == 3) code[at++] = 0x48;
            unsigned wide = log != 0;
            if (op <= LATS_ATOMIC_XOR) {
                code[at++] = (op - 1) * 8 + wide; code[at++] = 0x0f;
            } else if (op == LATS_ATOMIC_XCHG) {
                code[at++] = 0x86 + wide; code[at++] = 0x0f;
            } else if (op == LATS_ATOMIC_CMPXCHG || op == LATS_ATOMIC_XADD) {
                code[at++] = 0x0f;
                code[at++] = (op == LATS_ATOMIC_CMPXCHG ? 0xb0 : 0xc0) + wide;
                code[at++] = 0x0f;
            } else {
                code[at++] = (op <= LATS_ATOMIC_DEC ? 0xfe : 0xf6) + wide;
                code[at++] = 0x07 + (op - LATS_ATOMIC_INC) * 8;
            }
            memcpy(code + at, suffix, sizeof(suffix));
            for (unsigned a = 0; a < sizeof(values) / sizeof(values[0]); a++) {
                for (unsigned b = 0; b < sizeof(values) / sizeof(values[0]); b++) {
                    for (unsigned carry = 0; carry < 2; carry++) {
                        reset();
                        uint64_t memory = values[a], state[] = {values[(a + b) % 13],
                                                               values[b], 0x202 | carry};
                        uint64_t original = state[0], source = state[1];
                        ((void (*)(void *, uint64_t *))code)(&memory, state);
                        memcpy(ram[0], &values[a], 8);
                        env.regs[0] = original; env.regs[1] = source;
                        env.eflags = 0x202 | carry;
                        uint32_t d = op | log << 4 | 1 << 6 | 5 << 11;
                        env.aot_native_atomic = (LatxNativeAtomicRequest){0x1000, source, d, 0};
                        assert(latx_aot_atomic_complete(&env));
                        uint64_t actual; memcpy(&actual, ram[0], 8);
                        unsigned flag_mask = 0x8d5;
                        if (op == LATS_ATOMIC_AND || op == LATS_ATOMIC_OR || op == LATS_ATOMIC_XOR)
                            flag_mask &= ~CC_A; /* Architecturally undefined. */
                        if (actual != memory || env.regs[0] != state[0] ||
                            env.regs[1] != state[1] || ((env.eflags ^ state[2]) & flag_mask)) {
                            fprintf(stderr, "atomic mismatch op=%u bits=%u a=%#llx b=%#llx carry=%u\n",
                                    op, bits, (unsigned long long)values[a],
                                    (unsigned long long)source, carry);
                            abort();
                        }
                        cases++;
                    }
                }
            }
        }
    }
    assert(!munmap(code, 4096));
    printf("PASS: %u scalar atomic cases match native x86\n", cases);
}
#endif
int main(void)
{
#ifdef __x86_64__
    compare_native();
#endif
    const uint8_t xadd[] = {0xf0, 0x48, 0x0f, 0xc1, 0x17};
    assert(lats_aot_atomic_decode(xadd, sizeof(xadd)) == descriptor(LATS_ATOMIC_XADD, 3));
    const uint8_t high[] = {0xf0, 0x0f, 0xc0, 0x27};
    uint32_t d = lats_aot_atomic_decode(high, sizeof(high));
    assert(LATS_ATOMIC_HIGH_BYTE(d) && !LATS_ATOMIC_REGISTER(d) &&
           LATS_ATOMIC_BYTES(d) == 1 && LATS_ATOMIC_LENGTH(d) == 4);
    const uint8_t bad[] = {0xf0, 0x48, 0x01, 0xc0};
    assert(!lats_aot_atomic_decode(bad, sizeof(bad)));
    const uint8_t sib[] = {0xf0, 0x48, 0x83, 0x44, 0x8f, 0x10, 0xff};
    d = lats_aot_atomic_decode(sib, sizeof(sib));
    assert(LATS_ATOMIC_OPERATION(d) == LATS_ATOMIC_ADD && LATS_ATOMIC_LENGTH(d) == 7);
    for (unsigned match = 0; match < 2; match++) {
        reset();
        uint64_t previous = UINT64_C(0x123456789abcdef0), replacement = 77;
        memcpy(ram[0] + 1, &previous, 8);
        env.regs[R_EAX] = UINT64_C(0xffffeeee9abcdef0) + !match;
        env.regs[R_EDX] = UINT64_C(0xaaaabbbb12345678);
        env.eflags |= 0x8d5;
        const uint8_t paired[] = {0xf0, 0x0f, 0xc7, 0x0f};
        env.aot_native_atomic = (LatxNativeAtomicRequest){0x1001, replacement,
            lats_aot_atomic_decode(paired, sizeof(paired)), 0};
        assert(latx_aot_atomic_complete(&env));
        uint64_t stored; memcpy(&stored, ram[0] + 1, 8);
        assert(stored == (match ? replacement : previous));
        assert(env.regs[R_EAX] == (match ? UINT64_C(0xffffeeee9abcdef0) : (uint32_t)previous));
        assert(env.regs[R_EDX] == (match ? UINT64_C(0xaaaabbbb12345678) : (uint32_t)(previous >> 32)));
        assert(!!(env.eflags & CC_Z) == match && (env.eflags & (0x8d5 & ~CC_Z)) == (0x8d5 & ~CC_Z));
    }
    for (unsigned kind = 0; kind < 3; kind++) {
        reset(); ram[0][1] = 8;
        env.eflags |= 0x8d5;
        env.aot_native_atomic = (LatxNativeAtomicRequest){0x1001, 8,
            LATS_ATOMIC_BIT | (1 << 4) | (kind << 6) | (5 << 11), 0};
        assert(latx_aot_atomic_complete(&env));
        assert(ram[0][1] == (kind == 0 ? 8 : 0));
        assert((env.eflags & 0x8d5) == 0x8d5);
    }
    reset(); exclusive = false;
    env.aot_native_atomic.address = 0x1000; env.aot_native_atomic.value = 1;
    if (!setjmp(fault)) latx_helper_aot_atomic_request(&env, descriptor(LATS_ATOMIC_XADD, 3));
    assert(cpu.exception_index == EXCP_ATOMIC && env.aot_native_atomic.descriptor);
    exclusive = true;
    assert(latx_aot_atomic_complete(&env) && ram[0][0] == 1);

    for (unsigned log = 0; log < 4; log++) {
        unsigned bytes = 1U << log, bits = bytes * 8;
        for (unsigned offset = 0; offset < bytes; offset++) {
            reset(); memset(ram[0] + offset, 255, bytes);
            env.regs[2] = UINT64_C(0x1234567800000001);
            env.aot_native_atomic = (LatxNativeAtomicRequest){0x1000 + offset, 1,
                descriptor(LATS_ATOMIC_XADD, log), 0};
            assert(latx_aot_atomic_complete(&env));
            uint64_t result = 0; memcpy(&result, ram[0] + offset, bytes);
            assert(!result && (env.eflags & 0x8d5) == 0x55 &&
                   (env.eflags & ~UINT64_C(0x8d5)) == 0x602);
            uint64_t mask = UINT64_MAX >> (64 - bits);
            uint64_t expected = bits >= 32 ? mask : (UINT64_C(0x1234567800000001) & ~mask) | mask;
            assert(env.regs[2] == expected && env.eip == 0x400005 &&
                   env.aot_native_atomic.completed == 1 && !env.native_recovered);
            assert(!latx_aot_atomic_complete(&env));
        }
    }
    reset(); memset(ram[0] + 4092, 0xa5, 4); memset(ram[1], 0x5a, 4);
    writable[1] = false;
    env.aot_native_atomic = (LatxNativeAtomicRequest){0x1ffc, 99,
        descriptor(LATS_ATOMIC_XCHG, 3), 0};
    assert(setjmp(fault) == 0 ? (latx_aot_atomic_complete(&env), false) : true);
    assert(!env.aot_native_atomic.descriptor && env.eip == 0x400000 &&
           !env.aot_native_atomic.completed && !invalidations);
    for (unsigned n = 0; n < 4; n++) assert(ram[0][4092 + n] == 0xa5 && ram[1][n] == 0x5a);
    writable[1] = true;
    env.aot_native_atomic.descriptor = descriptor(LATS_ATOMIC_XCHG, 3);
    assert(latx_aot_atomic_complete(&env));
    assert(invalidations == 2);
    assert(env.regs[2] == UINT64_C(0x5a5a5a5aa5a5a5a5));
    uint64_t result = 0; memcpy(&result, ram[0] + 4092, 4);
    assert(result == 99); memcpy(&result, ram[1], 4); assert(!result);

    reset(); ram[0][0] = 7; writable[0] = false; env.regs[0] = 5;
    env.aot_native_atomic = (LatxNativeAtomicRequest){0x1000, 99,
        descriptor(LATS_ATOMIC_CMPXCHG, 3), 0};
    assert(setjmp(fault) == 0 ? (latx_aot_atomic_complete(&env), false) : true);
    assert(ram[0][0] == 7 && env.regs[0] == 5 && env.eflags == 0x602 &&
           env.eip == 0x400000 && !env.aot_native_atomic.descriptor);
    writable[0] = true; env.aot_native_atomic.descriptor = descriptor(LATS_ATOMIC_CMPXCHG, 3);
    assert(latx_aot_atomic_complete(&env));
    assert(ram[0][0] == 7 && env.regs[0] == 7 && (env.eflags & 0x8d5) == 0x91);
    puts("PASS: native atomic scalar results, permissions, unaligned and two-page commit");
}
'''
with tempfile.TemporaryDirectory(prefix="lats-native-atomic-") as temporary:
    folder = Path(temporary)
    (folder / "test.c").write_text(program)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-I", root / "include", "-I", root / "target/i386/latx/include",
                    folder / "test.c", "-o", folder / "test"], check=True)
    subprocess.run([folder / "test"], check=True)
