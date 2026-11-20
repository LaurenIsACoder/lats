/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * Exercise an unaligned MMIO read that crosses a guest page boundary.
 *
 * Modes:
 *   both  - both pages are inside the lockless edu BAR
 *   mixed - the first page is the lockless edu BAR and the second is the
 *           non-lockless unassigned region immediately after it
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define EDU_VENDOR "0x1234"
#define EDU_DEVICE "0x11e8"
#define PAGE_SIZE 4096UL

static int read_id(const char *device, const char *name, char *value,
                   size_t size)
{
    char path[512];
    int fd;
    ssize_t count;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", device,
             name);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    count = read(fd, value, size - 1);
    close(fd);
    if (count < 0) {
        return -1;
    }
    value[count] = '\0';
    return 0;
}

static int find_edu_resource(unsigned long long *start,
                             unsigned long long *end)
{
    DIR *dir = opendir("/sys/bus/pci/devices");
    struct dirent *entry;

    if (!dir) {
        return -1;
    }
    while ((entry = readdir(dir))) {
        char vendor[32], device[32], resource[512];
        FILE *file;

        if (entry->d_name[0] == '.' ||
            read_id(entry->d_name, "vendor", vendor, sizeof(vendor)) ||
            read_id(entry->d_name, "device", device, sizeof(device)) ||
            strncmp(vendor, EDU_VENDOR, strlen(EDU_VENDOR)) ||
            strncmp(device, EDU_DEVICE, strlen(EDU_DEVICE))) {
            continue;
        }
        snprintf(resource, sizeof(resource),
                 "/sys/bus/pci/devices/%s/resource", entry->d_name);
        file = fopen(resource, "r");
        if (file && fscanf(file, "%llx %llx", start, end) == 2) {
            fclose(file);
            closedir(dir);
            return 0;
        }
        if (file) {
            fclose(file);
        }
    }
    closedir(dir);
    return -1;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "both";
    unsigned long long loops = 1000000;
    unsigned long long start, end, target, map_base;
    /* Volatile forces every loop iteration to issue an MMIO read. */
    volatile uint32_t *ptr;
    uint32_t sink = 0;
    void *mapping;
    size_t map_size;
    int fd;

    if (argc > 2 && sscanf(argv[2], "%llu", &loops) != 1) {
        fprintf(stderr, "[CROSS-MMIO] ERROR: invalid loop count\n");
        return 2;
    }

    if (find_edu_resource(&start, &end)) {
        fprintf(stderr, "[CROSS-MMIO] ERROR: edu resource not found\n");
        return 2;
    }
    if (!strcmp(mode, "both")) {
        target = start + PAGE_SIZE - 2;
    } else if (!strcmp(mode, "mixed")) {
        target = end - 1;
    } else {
        fprintf(stderr, "[CROSS-MMIO] ERROR: mode must be both or mixed\n");
        return 2;
    }

    map_base = target & ~(PAGE_SIZE - 1);
    map_size = 2 * PAGE_SIZE;
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("[CROSS-MMIO] open /dev/mem");
        return 2;
    }
    mapping = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                   map_base);
    close(fd);
    if (mapping == MAP_FAILED) {
        perror("[CROSS-MMIO] mmap");
        return 2;
    }
    /* This pointer is intentionally volatile because it addresses MMIO. */
    ptr = (volatile uint32_t *)((char *)mapping + target - map_base);
    for (unsigned long long i = 0; i < loops; i++) {
        sink ^= *ptr;
    }
    for (unsigned long long i = 0; i < loops; i++) {
        *ptr = sink;
    }
    printf("[CROSS-MMIO] mode=%s reads=%llu writes=%llu start=0x%llx "
           "end=0x%llx sink=0x%x DONE\n", mode, loops, loops, start, end,
           sink);
    return 0;
}
