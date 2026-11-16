/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * bql-stress: guest-side BQL contention generator for LATS system mode.
 *
 * Two contention roles:
 *  - "mmio": hammer QEMU "edu" PCI device BAR0 registers (each access is an
 *    MMIO exit that reaches device emulation through the BQL path).
 *  - "shootdown": flip mprotect() on a shared region while peer threads
 *    touch its pages, forcing TLB shootdown IPIs between vCPUs (APIC ICR
 *    MMIO writes + interrupt handling).
 *
 * Build (on x86 host):  gcc -O2 -static -pthread -o bql-stress bql-stress.c
 * Run (in guest):       bql-stress <duration_sec> <mmio_threads> <shootdown_threads>
 *
 * Result lines are printed with the [BQL-STRESS] prefix for the host driver.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define EDU_VENDOR "0x1234"
#define EDU_DEVICE "0x11e8"

#define EDU_REG_IDENT  0x00
#define EDU_REG_STATUS 0x20

#define SD_REGION_SIZE (64UL * 1024 * 1024)
#define PAGE 4096

static volatile int stop_flag;
static void *edu_bar;
static char *sd_region;

struct counter {
    unsigned long long ops;
};

static long read_text(const char *path, char *buf, long size)
{
    int fd = open(path, O_RDONLY);
    long n;
    if (fd < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return n;
}

static void *find_edu_bar(void)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    struct dirent *de;
    static char respath[512];

    if (!d)
        return NULL;
    while ((de = readdir(d))) {
        char path[512], vendor[64], device[64];
        int fd;
        void *bar;

        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", de->d_name);
        if (read_text(path, vendor, sizeof(vendor)) < 0)
            continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", de->d_name);
        if (read_text(path, device, sizeof(device)) < 0)
            continue;
        if (strncmp(vendor, EDU_VENDOR, strlen(EDU_VENDOR)) ||
            strncmp(device, EDU_DEVICE, strlen(EDU_DEVICE)))
            continue;

        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/enable", de->d_name);
        fd = open(path, O_WRONLY);
        if (fd >= 0) {
            if (write(fd, "1", 1) < 0)
                perror("enable write");
            close(fd);
        }
        snprintf(respath, sizeof(respath), "/sys/bus/pci/devices/%s/resource0", de->d_name);
        fd = open(respath, O_RDWR | O_SYNC);
        if (fd < 0) {
            perror(respath);
            continue;
        }
        bar = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (bar == MAP_FAILED) {
            perror("mmap resource0");
            continue;
        }
        closedir(d);
        fprintf(stderr, "[BQL-STRESS] edu_bar=%s\n", respath);
        return bar;
    }
    closedir(d);
    return NULL;
}

static void *mmio_worker(void *arg)
{
    struct counter *c = arg;
    volatile uint32_t *bar = edu_bar;
    uint32_t v = 0;

    while (!stop_flag) {
        bar[EDU_REG_STATUS / 4] = 0;
        v ^= bar[EDU_REG_IDENT / 4];
        c->ops++;
    }
    (void)v;
    return NULL;
}

static void *sd_flipper(void *arg)
{
    struct counter *c = arg;

    while (!stop_flag) {
        mprotect(sd_region, SD_REGION_SIZE, PROT_READ);
        mprotect(sd_region, SD_REGION_SIZE, PROT_READ | PROT_WRITE);
        c->ops++;
    }
    return NULL;
}

static void *sd_toucher(void *arg)
{
    struct counter *c = arg;
    unsigned long i = 0;
    volatile char sink = 0;

    while (!stop_flag) {
        sink += sd_region[i];
        i += PAGE;
        if (i >= SD_REGION_SIZE)
            i = 0;
        c->ops++;
    }
    (void)sink;
    return NULL;
}

int main(int argc, char **argv)
{
    int duration = argc > 1 ? atoi(argv[1]) : 10;
    int n_mmio = argc > 2 ? atoi(argv[2]) : 2;
    int n_sd = argc > 3 ? atoi(argv[3]) : 2;
    int i, n_threads;
    double elapsed;

    if (duration <= 0 || n_mmio < 0 || n_sd < 0 || n_mmio + n_sd <= 0) {
        fprintf(stderr, "[BQL-STRESS] ERROR: bad arguments\n");
        return 2;
    }
    n_threads = n_mmio + n_sd;
    pthread_t *tids;
    struct counter *counters;
    struct timespec ts;

    printf("[BQL-STRESS] duration=%d mmio_threads=%d sd_threads=%d\n",
           duration, n_mmio, n_sd);
    fflush(stdout);

    if (n_mmio > 0) {
        edu_bar = find_edu_bar();
        if (!edu_bar) {
            fprintf(stderr, "[BQL-STRESS] ERROR: edu device not found\n");
            return 2;
        }
    }

    if (n_sd > 0) {
        sd_region = mmap(NULL, SD_REGION_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (sd_region == MAP_FAILED) {
            perror("mmap sd_region");
            return 2;
        }
        memset(sd_region, 1, SD_REGION_SIZE);
    }

    tids = calloc(n_threads, sizeof(*tids));
    counters = calloc(n_threads, sizeof(*counters));

    for (i = 0; i < n_mmio; i++)
        pthread_create(&tids[i], NULL, mmio_worker, &counters[i]);
    for (i = 0; i < n_sd; i++)
        pthread_create(&tids[n_mmio + i], NULL,
                       i == 0 ? sd_flipper : sd_toucher,
                       &counters[n_mmio + i]);

    ts.tv_sec = duration;
    ts.tv_nsec = 0;
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        nanosleep(&ts, NULL);
        stop_flag = 1;

        for (i = 0; i < n_threads; i++)
            pthread_join(tids[i], NULL);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    }

    {
        unsigned long long mmio_ops = 0, sd_ops = 0;
        for (i = 0; i < n_mmio; i++)
            mmio_ops += counters[i].ops;
        for (i = n_mmio; i < n_threads; i++)
            sd_ops += counters[i].ops;
        printf("[BQL-STRESS] elapsed=%.3f\n", elapsed);
        printf("[BQL-STRESS] mmio_ops=%llu mmio_ops_per_sec=%llu\n",
               mmio_ops, (unsigned long long)(mmio_ops / elapsed));
        printf("[BQL-STRESS] shootdown_ops=%llu shootdown_ops_per_sec=%llu\n",
               sd_ops, (unsigned long long)(sd_ops / elapsed));
        printf("[BQL-STRESS] total_ops=%llu\n", mmio_ops + sd_ops);
    }
    fflush(stdout);
    return 0;
}
