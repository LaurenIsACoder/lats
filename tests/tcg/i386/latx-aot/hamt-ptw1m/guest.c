/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <unistd.h>

#define PAGE_BYTES 4096UL
#define HUGE_BYTES (2UL * 1024 * 1024)
#define TEST_PAGES 256
#define ITERATIONS 3

static void finish(int ok)
{
    printf("HAMT_BOUNDARY_RESULT=%s\n", ok ? "PASS" : "FAIL");
    fflush(NULL);
    sync();
    reboot(RB_POWER_OFF);
    perror("reboot");
    for (;;) {
        pause();
    }
}

static int physical_region(void *base, uint64_t *physical)
{
    uint64_t entries[TEST_PAGES];
    uint64_t pfn_mask = (1ULL << 55) - 1;
    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    off_t off = ((uintptr_t)base / PAGE_BYTES) * sizeof(entries[0]);
    if (fd < 0) {
        perror("open pagemap");
        return -1;
    }
    ssize_t n = pread(fd, entries, sizeof(entries), off);
    close(fd);
    if (n != sizeof(entries)) {
        fprintf(stderr, "pagemap read failed: %zd: %s\n", n, strerror(errno));
        return -1;
    }
    uint64_t first = entries[0] & pfn_mask;
    if (!first || (first & (TEST_PAGES - 1))) {
        fprintf(stderr, "PFN unavailable or not 1M-aligned: %#" PRIx64 "\n", first);
        return -1;
    }
    for (size_t i = 0; i < TEST_PAGES; i++) {
        if (!(entries[i] & (1ULL << 63)) || (entries[i] & pfn_mask) != first + i) {
            fprintf(stderr, "noncontiguous pagemap entry at page %zu: %#" PRIx64 "\n",
                    i, entries[i]);
            return -1;
        }
    }
    *physical = first * PAGE_BYTES;
    return 0;
}

static uint32_t expected(unsigned iteration, unsigned page)
{
    return 0x12340000U + iteration * TEST_PAGES + page;
}

static void write_code(unsigned char *p, uint32_t value)
{
    p[0] = 0xb8; /* mov imm32, %eax; ret */
    memcpy(p + 1, &value, sizeof(value));
    p[5] = 0xc3;
    __builtin___clear_cache((char *)p, (char *)p + 6);
}

static int execute(unsigned char *base, unsigned iteration, unsigned pages)
{
    for (unsigned i = 0; i < pages; i++) {
        uint32_t (*fn)(void) = (void *)(base + i * PAGE_BYTES);
        uint32_t actual = fn();
        uint32_t want = expected(iteration, i);
        if (actual != want) {
            fprintf(stderr, "FAIL iteration=%u page=%u got=%#x expected=%#x\n",
                    iteration, i, actual, want);
            return -1;
        }
        if (i == 254 || i == 255) {
            printf("EXEC iteration=%u pages=%u\n", iteration, i + 1);
        }
    }
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("HAMT_BOUNDARY_BEGIN pid=%ld\n", (long)getpid());
    if (getpid() != 1) {
        fprintf(stderr, "Run as guest init; refusing to power off this system.\n");
        return 2;
    }
    if (mount("proc", "/proc", "proc", 0, NULL) && errno != EBUSY) {
        perror("mount /proc");
        finish(0);
    }
    void *mapping = mmap(NULL, 2 * HUGE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        finish(0);
    }
    unsigned char *base = (void *)(((uintptr_t)mapping + HUGE_BYTES - 1) &
                                   ~(HUGE_BYTES - 1));
    if (madvise(base, HUGE_BYTES, MADV_HUGEPAGE)) {
        perror("madvise MADV_HUGEPAGE");
        finish(0);
    }
    memset(base, 0x90, HUGE_BYTES);
    for (unsigned i = 0; i < TEST_PAGES; i++) {
        write_code(base + i * PAGE_BYTES, expected(0, i));
    }
    uint64_t physical;
    if (physical_region(base, &physical)) {
        fprintf(stderr, "FAIL: physical contiguity prerequisite not satisfied\n");
        finish(0);
    }
    printf("PHYSICAL_1M_BASE=%#" PRIx64 " pages=%u\n", physical, TEST_PAGES);
    printf("CONTROL_255_BEGIN\n");
    if (execute(base, 0, TEST_PAGES - 1)) {
        finish(0);
    }
    for (unsigned i = 0; i < TEST_PAGES - 1; i++) {
        write_code(base + i * PAGE_BYTES, expected(1, i));
    }
    if (execute(base, 1, TEST_PAGES - 1)) {
        finish(0);
    }
    printf("CONTROL_255_PASS\n");

    write_code(base + (TEST_PAGES - 1) * PAGE_BYTES,
               expected(1, TEST_PAGES - 1));
    for (unsigned iteration = 1; iteration <= ITERATIONS; iteration++) {
        printf("EXEC_BEGIN iteration=%u pages=256\n", iteration);
        if (execute(base, iteration, TEST_PAGES)) {
            finish(0);
        }
        if (iteration < ITERATIONS) {
            printf("MODIFY_BEGIN iteration=%u\n", iteration + 1);
            for (unsigned i = 0; i < TEST_PAGES; i++) {
                write_code(base + i * PAGE_BYTES, expected(iteration + 1, i));
            }
            printf("MODIFY_DONE iteration=%u pages=%u\n", iteration + 1, TEST_PAGES);
        }
    }
    printf("PASS: 256 code pages across %u execution rounds\n", ITERATIONS);
    finish(1);
    return 0;
}
