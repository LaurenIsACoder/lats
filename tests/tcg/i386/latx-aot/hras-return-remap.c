/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <unistd.h>
#include <time.h>

/* Build with -O2 -static -no-pie -mno-red-zone; add -DINDIRECT_CALL for CALL r11.
 * Native check: ./hras-return-remap private_smc 101
 * PID 1 accepts the existing ras-tp-bench BENCH_BEGIN/BENCH_END protocol. */
static uint64_t clock_ns(void)
{
    struct timespec t;
    assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

extern uint64_t owner_a(uint64_t), owner_b(uint64_t);
extern unsigned char shared_return[], return_imm[];
/* Keep the CALL page fixed while replacing only the return mapping.
 * The second build uses an indirect CALL with exactly the same page split. */
__asm__(
    ".text\n.balign 4096\n"
#ifdef INDIRECT_CALL
    ".space 4082,0x90\n"
    ".global owner_a,owner_b,shared_return,return_imm\n"
    "owner_a: nop\nowner_b: movabs $private_leaf,%r11\ncall *%r11\n"
#else
    ".space 4090,0x90\n"
    ".global owner_a,owner_b,shared_return,return_imm\n"
    "owner_a: nop\nowner_b: call private_leaf\n"
#endif
    "shared_return: .byte 0x48,0x83,0xc0\nreturn_imm: .byte 1\nret\n"
    ".balign 4096\nprivate_leaf: lea 7(%rdi,%rdi,2),%rax\nret\n"
);
static uint64_t return_only(uint64_t x)
{
    __asm__ volatile("call *%1" : "+a"(x) : "r"(shared_return)
                     : "rcx", "rdx", "rsi", "rdi", "r8", "r9",
                       "r10", "r11", "cc", "memory");
    return x;
}
static int backing[2], current;
static unsigned char *aliases[2];
static size_t page_size;
static void *return_page;
static void prepare_mappings(void)
{
    page_size = sysconf(_SC_PAGESIZE);
    assert(page_size == 4096);
    return_page = (void *)((uintptr_t)return_imm & ~(page_size - 1));
    size_t offset = (uintptr_t)return_imm - (uintptr_t)return_page;
    for (unsigned i = 0; i < 2; i++) {
        backing[i] = memfd_create("hras-return", 0);
        assert(backing[i] >= 0 && !ftruncate(backing[i], page_size));
        aliases[i] = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                          MAP_SHARED, backing[i], 0);
        assert(aliases[i] != MAP_FAILED);
        memcpy(aliases[i], return_page, page_size);
        aliases[i][offset] = i + 1;
        assert(!mprotect(aliases[i], page_size, PROT_READ | PROT_EXEC));
    }
    assert(mmap(return_page, page_size, PROT_READ | PROT_EXEC,
                MAP_FIXED | MAP_SHARED, backing[0], 0) == return_page);
    current = 1;
}
static void set_immediate(unsigned value)
{
    assert(value == 1 || value == 2);
    if (current == value) return;
    assert(mmap(return_page, page_size, PROT_READ | PROT_EXEC,
                MAP_FIXED | MAP_SHARED, backing[value - 1], 0) == return_page);
    current = value;
    size_t offset = (uintptr_t)return_imm - (uintptr_t)return_page;
    /* Both physical backing pages remain mapped and unmodified. */
    assert(aliases[0][offset] == 1 && aliases[1][offset] == 2);
}
static uint64_t run(uint64_t n, int modify)
{
    uint64_t x = 42;
    for (uint64_t i = 0; i < n; i++) {
        unsigned add = modify ? 1 + (i & 1) : 1;
        set_immediate(add);
        assert(return_only(123) == 123 + add);
        for (unsigned j = 0; j < 4; j++) {
            uint64_t expected = 3 * x + 7 + add;
            x = j & 1 ? owner_b(x) : owner_a(x);
            assert(x == expected);
        }
    }
    return x;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getpid() == 1) {
        mount("proc", "/proc", "proc", 0, NULL);
        mount("sysfs", "/sys", "sysfs", 0, NULL);
    }
    prepare_mappings();
    if (argc == 3) {
        printf("%016" PRIx64 "\n", run(strtoull(argv[2], NULL, 10),
                                      !strcmp(argv[1], "private_smc")));
        return 0;
    }
    puts("BENCH_READY");
    char name[64];
    uint64_t n;
    while (scanf("%63s %" SCNu64, name, &n) == 2) {
        assert(!strcmp(name, "private_bind") || !strcmp(name, "private_smc"));
        printf("BENCH_BEGIN %s %" PRIu64 "\n", name, n);
        uint64_t start = clock_ns();
        uint64_t value = run(n, !strcmp(name, "private_smc"));
        uint64_t elapsed = clock_ns() - start;
        printf("BENCH_END {\"case\":\"%s\",\"iterations\":%" PRIu64
               ",\"ns\":%" PRIu64 ",\"checksum\":\"%016" PRIx64 "\"}\n", name, n, elapsed, value);
    }
    return 0;
}
