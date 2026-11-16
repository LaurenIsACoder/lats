/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Repeatedly invalidate a direct jump's destination while keeping its source.
 * Build on x86-64: cc -O2 -Wall -Wextra -o tb-relink-bench tb-relink-bench.c
 * Run: [rounds [calls [--alternate-targets]]]. The optional mode alternates
 * two unchanged file-backed target pages at the same virtual address.
 * Each round prints ns/call and checks every result. No runner is required.
 */
#define _GNU_SOURCE
#include <errno.h>
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
    if (errno || !*text || *end || !value || value > 1000000000) {
        fprintf(stderr, "invalid count: %s\n", text);
        exit(1);
    }
    return value;
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? parse_count(argv[1]) : 64;
    unsigned calls = argc > 2 ? parse_count(argv[2]) : 2000000;
    int alternate = argc == 4 && !strcmp(argv[3], "--alternate-targets");
    long page = sysconf(_SC_PAGESIZE);
    if (argc > 4 || (argc == 4 && !alternate) ||
        page < 4096 || page > INT32_MAX) {
        fprintf(stderr, "usage: %s [rounds [calls [--alternate-targets]]]\n",
                argv[0]);
        return 1;
    }
    uint8_t *code = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    /* Source and destination occupy different physical pages. */
    code[0] = 0xe9;                   /* jmp rel32 */
    int32_t displacement = page - 5;
    memcpy(code + 1, &displacement, sizeof(displacement));
    code[page] = 0xb8;                /* mov eax, imm32 */
    code[page + 5] = 0xc3;            /* ret */
    if (mprotect(code, page, PROT_READ | PROT_EXEC)) {
        perror("mprotect source");
        return 1;
    }
    FILE *targets = NULL;
    uint8_t *alias = MAP_FAILED;
    if (alternate) {
        targets = tmpfile();
        if (!targets || ftruncate(fileno(targets), 2 * page)) {
            perror("target file");
            return 1;
        }
        for (unsigned i = 0; i < 2; i++) {
            uint8_t body[] = { 0xb8, 0, 0, 0, 0, 0xc3 };
            uint32_t value = i + 1;
            memcpy(body + 1, &value, sizeof(value));
            if (pwrite(fileno(targets), body, sizeof(body), i * page) !=
                sizeof(body)) {
                perror("target write");
                return 1;
            }
        }
        /* Keep both target mappings alive while the executable alias moves. */
        alias = mmap(NULL, 2 * page, PROT_READ, MAP_SHARED, fileno(targets), 0);
        if (alias == MAP_FAILED) {
            perror("target alias");
            return 1;
        }
    }
    int (*volatile entry)(void) = (int (*)(void))code;
    uint64_t checksum = 0;
    for (unsigned r = 0; r < rounds; r++) {
        uint32_t expected = alternate ? (r & 1) + 1 : r + 1;
        if (alternate) {
            if (mmap(code + page, page, PROT_READ | PROT_EXEC,
                     MAP_SHARED | MAP_FIXED, fileno(targets), (r & 1) * page) ==
                MAP_FAILED) {
                perror("target remap");
                return 1;
            }
        } else {
            if (mprotect(code + page, page, PROT_READ | PROT_WRITE)) {
                perror("mprotect writable");
                return 1;
            }
            memcpy(code + page + 1, &expected, sizeof(expected));
            if (mprotect(code + page, page, PROT_READ | PROT_EXEC)) {
                perror("mprotect executable");
                return 1;
            }
        }
        uint64_t begin = now_ns();
        uint64_t sum = 0;
        for (unsigned i = 0; i < calls; i++) {
            int value = entry();
            if (value != (int)expected) {
                fprintf(stderr, "FAIL round=%u call=%u got=%d expected=%u\n",
                        r, i, value, expected);
                return 1;
            }
            sum += value;
        }
        uint64_t elapsed = now_ns() - begin;
        checksum += sum;
        printf("R %s %u %" PRIu64 " %" PRIu64 " ns/call=%.3f\n",
               alternate ? "alternate" : "relink", r, elapsed, sum,
               (double)elapsed / calls);
        fflush(stdout);
    }
    printf("PASS rounds=%u calls=%u checksum=%" PRIu64 "\n",
           rounds, calls, checksum);
    if (alternate) {
        if (munmap(alias, 2 * page) || fclose(targets)) {
            perror("target cleanup");
            return 1;
        }
    }
    return munmap(code, 2 * page) != 0;
}
