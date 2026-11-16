/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Dense code pages expose per-TB write-protection costs. The alias checks
 * deliberately create a writable mapping only AFTER executing the code.
 * Build: cc -O2 -Wall -Wextra -o smc-protect-bench smc-protect-bench.c
 * Run on x86-64 directly or through any translator: [pages [passes]].
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
#include <time.h>
#include <unistd.h>

#if !defined(__x86_64__)
#error This test generates x86-64 instructions.
#endif

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static unsigned parse_count(const char *text)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !*text || *end || !value || value > 65536) {
        fprintf(stderr, "invalid count: %s\n", text);
        exit(1);
    }
    return value;
}

int main(int argc, char **argv)
{
    unsigned pages = argc > 1 ? parse_count(argv[1]) : 128;
    unsigned passes = argc > 2 ? parse_count(argv[2]) : 3;
    long page = sysconf(_SC_PAGESIZE);
    if (argc > 3 || page < 4096) {
        fprintf(stderr, "usage: %s [pages [passes]]\n", argv[0]);
        return 1;
    }
    size_t length = (size_t)pages * page;
    /* An unlinked regular file works with old kernels without memfd_create. */
    char path[] = "/tmp/smc-protect-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    unlink(path);
    if (ftruncate(fd, length)) {
        perror("ftruncate");
        return 1;
    }
    uint8_t *code = mmap((void *)0x600000000000ULL, length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    if (code == MAP_FAILED) {
        perror("mmap code");
        return 1;
    }
    printf("CODE_ADDRESS %p\n", (void *)code);
    /* 16-byte functions, each an independent translation block. */
    unsigned functions = length / 16;
    for (unsigned f = 0; f < functions; f++) {
        uint32_t value = f + 1;
        code[f * 16] = 0xb8;          /* mov eax, imm32 */
        memcpy(code + f * 16 + 1, &value, sizeof(value));
        code[f * 16 + 5] = 0xc3;      /* ret */
    }
    if (mprotect(code, length, PROT_READ | PROT_EXEC)) {
        perror("mprotect");
        return 1;
    }
    uint8_t *early_alias = mmap(NULL, length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, 0);
    if (early_alias == MAP_FAILED) {
        perror("mmap early alias");
        return 1;
    }
    /* Fill writable translations before the pages become code-protected. */
    for (unsigned p = 0; p < pages; p++) {
        volatile uint8_t *byte = early_alias + (size_t)p * page;
        *byte = *byte;
    }
    for (unsigned pass = 0; pass < passes; pass++) {
        uint64_t begin = now_ns(), sum = 0;
        for (unsigned f = 0; f < functions; f++) {
            int value = ((int (*)(void))(code + f * 16))();
            if (value != (int)f + 1) {
                fprintf(stderr, "FAIL pass=%u function=%u got=%d\n", pass, f, value);
                return 1;
            }
            sum += value;
        }
        uint64_t elapsed = now_ns() - begin;
        printf("R dense %u %" PRIu64 " %" PRIu64 " ns/block=%.3f\n",
               pass, elapsed, sum, (double)elapsed / functions);
    }
    for (unsigned p = 0; p < pages; p++) {
        uint32_t expected = 2000000 + p;
        memcpy(early_alias + (size_t)p * page + 17, &expected, sizeof(expected));
        int value = ((int (*)(void))(code + (size_t)p * page + 16))();
        if (value != (int)expected) {
            fprintf(stderr, "FAIL early alias page=%u got=%d expected=%u\n",
                    p, value, expected);
            return 1;
        }
    }
    uint8_t *alias = mmap(NULL, length, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
    if (alias == MAP_FAILED) {
        perror("mmap alias");
        return 1;
    }
    /* Touch the new mapping before changing code, then check stale TBs. */
    for (unsigned p = 0; p < pages; p++) {
        volatile uint8_t old = alias[(size_t)p * page];
        (void)old;
        uint32_t expected = 1000000 + p;
        memcpy(alias + (size_t)p * page + 1, &expected, sizeof(expected));
        int value = ((int (*)(void))(code + (size_t)p * page))();
        if (value != (int)expected) {
            fprintf(stderr, "FAIL alias page=%u got=%d expected=%u\n", p, value, expected);
            return 1;
        }
    }
    printf("PASS pages=%u functions=%u passes=%u alias-writes=%u\n",
           pages, functions, passes, 2 * pages);
    return (munmap(early_alias, length) || munmap(alias, length) ||
            munmap(code, length) || close(fd)) != 0;
}
