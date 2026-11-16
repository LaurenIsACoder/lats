/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * Guest-side HPET main-counter read benchmark for LATX system mode.
 *
 * Build on x86_64:
 *   gcc -O2 -static -pthread -o hpet-read-stress hpet-read-stress.c
 *
 * Run as root in the guest:
 *   hpet-read-stress [duration_sec] [threads]
 *
 * The program maps the HPET MMIO page through /dev/mem, enables the main
 * counter if needed, and checks that repeated 64-bit samples never move
 * backwards.  Each sample uses the standard high-low-high 32-bit sequence.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define HPET_BASE 0xfed00000ULL
#define HPET_LEN  0x400
#define HPET_CFG  0x010
#define HPET_COUNTER_LO 0x0f0
#define HPET_COUNTER_HI 0x0f4
#define HPET_CFG_ENABLE 0x001

/* Volatile is required because every load must reach the MMIO device. */
static volatile uint32_t *hpet;
static atomic_bool stop_flag;

struct worker_result {
    uint64_t samples;
    uint64_t backwards;
    uint64_t first_backward_previous;
    uint64_t first_backward_current;
    uint64_t max_backward_delta;
    uint64_t first;
    uint64_t last;
};

static uint64_t read_counter(void)
{
    uint32_t hi1, lo, hi2;

    do {
        hi1 = hpet[HPET_COUNTER_HI / 4];
        lo = hpet[HPET_COUNTER_LO / 4];
        hi2 = hpet[HPET_COUNTER_HI / 4];
    } while (hi1 != hi2);

    return ((uint64_t)hi2 << 32) | lo;
}

static __attribute__((noinline, noclone))
uint32_t read_fake_f0(const uint8_t *base)
{
    const _Atomic uint32_t *value =
        (const _Atomic uint32_t *)(base + HPET_COUNTER_LO);

    return atomic_load_explicit(value, memory_order_relaxed);
}

static __attribute__((noinline, noclone))
uint32_t read_fake_f4(const uint8_t *base)
{
    const _Atomic uint32_t *value =
        (const _Atomic uint32_t *)(base + HPET_COUNTER_HI);

    return atomic_load_explicit(value, memory_order_relaxed);
}

static void *worker(void *opaque)
{
    struct worker_result *result = opaque;
    uint64_t previous = read_counter();

    result->first = previous;
    while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
        uint64_t current = read_counter();

        if (current < previous) {
            uint64_t delta = previous - current;

            if (!result->backwards) {
                result->first_backward_previous = previous;
                result->first_backward_current = current;
            }
            if (delta > result->max_backward_delta) {
                result->max_backward_delta = delta;
            }
            result->backwards++;
        }
        previous = current;
        result->samples++;
    }
    result->last = previous;
    return NULL;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) +
           (end->tv_nsec - start->tv_nsec) / 1e9;
}

int main(int argc, char **argv)
{
    int duration = argc > 1 ? atoi(argv[1]) : 20;
    int threads = argc > 2 ? atoi(argv[2]) : 4;
    struct worker_result *results;
    pthread_t *tids;
    struct timespec start, end;
    uint8_t fake[HPET_LEN] __attribute__((aligned(64))) = { 0 };
    _Atomic uint32_t *fake_f0 =
        (_Atomic uint32_t *)(fake + HPET_COUNTER_LO);
    _Atomic uint32_t *fake_f4 =
        (_Atomic uint32_t *)(fake + HPET_COUNTER_HI);
    uint64_t samples = 0, backwards = 0;
    int fd, i, enabled_by_test = 0, remap_failed = 0;
    double elapsed;

    if (duration <= 0 || threads <= 0 || threads > 256) {
        fprintf(stderr, "[HPET-STRESS] invalid arguments\n");
        return 2;
    }

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "[HPET-STRESS] open /dev/mem: %s\n",
                strerror(errno));
        return 2;
    }
    hpet = mmap(NULL, HPET_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                HPET_BASE);
    close(fd);
    if (hpet == MAP_FAILED) {
        fprintf(stderr, "[HPET-STRESS] mmap HPET: %s\n", strerror(errno));
        return 2;
    }

    if (!(hpet[HPET_CFG / 4] & HPET_CFG_ENABLE)) {
        hpet[HPET_CFG / 4] |= HPET_CFG_ENABLE;
        __sync_synchronize();
        enabled_by_test = 1;
    }

    atomic_store_explicit(fake_f0, 0x13579bdf, memory_order_relaxed);
    atomic_store_explicit(fake_f4, 0x2468ace0, memory_order_relaxed);
    if (read_fake_f0(fake) !=
            atomic_load_explicit(fake_f0, memory_order_relaxed) ||
        read_fake_f4(fake) !=
            atomic_load_explicit(fake_f4, memory_order_relaxed)) {
        fprintf(stderr, "[HPET-STRESS] non-HPET fallback failed\n");
        return 2;
    }
    printf("[HPET-STRESS] non_hpet_fallback=pass\n");

    results = calloc(threads, sizeof(*results));
    tids = calloc(threads, sizeof(*tids));
    if (!results || !tids) {
        fprintf(stderr, "[HPET-STRESS] allocation failed\n");
        return 2;
    }

    atomic_init(&stop_flag, false);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < threads; i++) {
        if (pthread_create(&tids[i], NULL, worker, &results[i])) {
            fprintf(stderr, "[HPET-STRESS] pthread_create failed\n");
            return 2;
        }
    }

    for (i = 0; i < duration; i += 2) {
        int seconds = duration - i < 2 ? duration - i : 2;
        struct timespec tick = { .tv_sec = seconds };

        nanosleep(&tick, NULL);
        printf("[HPET-STRESS] TICK %d\n", i + (int)tick.tv_sec);
        fflush(stdout);
    }
    atomic_store_explicit(&stop_flag, true, memory_order_relaxed);

    for (i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
        if (results[i].backwards) {
            printf("[HPET-STRESS] thread=%d first_backward=%llu->%llu "
                   "max_delta=%llu backwards=%llu\n",
                   i,
                   (unsigned long long)results[i].first_backward_previous,
                   (unsigned long long)results[i].first_backward_current,
                   (unsigned long long)results[i].max_backward_delta,
                   (unsigned long long)results[i].backwards);
        }
        samples += results[i].samples;
        backwards += results[i].backwards;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsed = elapsed_seconds(&start, &end);

    printf("[HPET-STRESS] duration=%.3f threads=%d enabled_by_test=%d\n",
           elapsed, threads, enabled_by_test);
    printf("[HPET-STRESS] samples=%llu samples_per_sec=%.0f backwards=%llu\n",
           (unsigned long long)samples, samples / elapsed,
           (unsigned long long)backwards);

    {
        void *old_mapping = (void *)hpet;
        uint8_t *replacement;
        _Atomic uint32_t *replacement_f0;
        _Atomic uint32_t *replacement_f4;

        munmap(old_mapping, HPET_LEN);
        replacement = mmap(old_mapping, HPET_LEN, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (replacement == MAP_FAILED) {
            fprintf(stderr, "[HPET-STRESS] replacement mmap failed: %s\n",
                    strerror(errno));
            remap_failed = 1;
        } else {
            replacement_f0 = (_Atomic uint32_t *)(replacement +
                                                   HPET_COUNTER_LO);
            replacement_f4 = (_Atomic uint32_t *)(replacement +
                                                   HPET_COUNTER_HI);
            atomic_store_explicit(replacement_f0, 0x89abcdef,
                                  memory_order_relaxed);
            atomic_store_explicit(replacement_f4, 0x10293847,
                                  memory_order_relaxed);
            if (read_fake_f0(replacement) != 0x89abcdef ||
                read_fake_f4(replacement) != 0x10293847) {
                fprintf(stderr, "[HPET-STRESS] remap fallback failed\n");
                remap_failed = 1;
            } else {
                printf("[HPET-STRESS] remap_fallback=pass\n");
            }
            munmap(replacement, HPET_LEN);
        }
    }
    free(tids);
    free(results);
    return backwards || remap_failed ? 1 : 0;
}
