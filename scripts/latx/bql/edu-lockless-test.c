/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * Verify per-register lockless dispatch for the QEMU edu PCI device.
 *
 * safe:   read 0x00 and write 0x20, both implemented without shared
 *         non-atomic state.
 * unsafe: read 0x04, which relies on the BQL for serialization.
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
#define EDU_REG_IDENT  0x00
#define EDU_REG_INVERT 0x04
#define EDU_REG_STATUS 0x20
#define PAGE_SIZE 4096

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

static void *map_edu_bar(void)
{
    DIR *dir = opendir("/sys/bus/pci/devices");
    struct dirent *entry;

    if (!dir) {
        return NULL;
    }
    while ((entry = readdir(dir))) {
        char vendor[32], device[32], enable[512], resource[512];
        int fd;
        void *mapping;

        if (entry->d_name[0] == '.' ||
            read_id(entry->d_name, "vendor", vendor, sizeof(vendor)) ||
            read_id(entry->d_name, "device", device, sizeof(device)) ||
            strncmp(vendor, EDU_VENDOR, strlen(EDU_VENDOR)) ||
            strncmp(device, EDU_DEVICE, strlen(EDU_DEVICE))) {
            continue;
        }
        snprintf(enable, sizeof(enable),
                 "/sys/bus/pci/devices/%s/enable", entry->d_name);
        fd = open(enable, O_WRONLY);
        if (fd >= 0) {
            ssize_t ignored = write(fd, "1", 1);

            (void)ignored;
            close(fd);
        }
        snprintf(resource, sizeof(resource),
                 "/sys/bus/pci/devices/%s/resource0", entry->d_name);
        fd = open(resource, O_RDWR | O_SYNC);
        if (fd < 0) {
            continue;
        }
        mapping = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                       fd, 0);
        close(fd);
        closedir(dir);
        return mapping == MAP_FAILED ? NULL : mapping;
    }
    closedir(dir);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "safe";
    unsigned long long loops = 500000;
    /* Volatile forces every loop iteration to issue an MMIO access. */
    volatile uint32_t *bar = map_edu_bar();
    uint32_t value = 0;

    if (argc > 2 && sscanf(argv[2], "%llu", &loops) != 1) {
        fprintf(stderr, "[EDU-LOCKLESS] ERROR: invalid loop count\n");
        return 2;
    }
    if (!bar || loops == 0) {
        fprintf(stderr, "[EDU-LOCKLESS] ERROR: invalid setup\n");
        return 2;
    }

    if (!strcmp(mode, "safe")) {
        for (unsigned long long i = 0; i < loops; i++) {
            bar[EDU_REG_STATUS / 4] = 0;
            value ^= bar[EDU_REG_IDENT / 4];
        }
    } else if (!strcmp(mode, "unsafe")) {
        for (unsigned long long i = 0; i < loops; i++) {
            value ^= bar[EDU_REG_INVERT / 4];
        }
    } else {
        fprintf(stderr, "[EDU-LOCKLESS] ERROR: mode must be safe or unsafe\n");
        return 2;
    }

    printf("[EDU-LOCKLESS] mode=%s loops=%llu value=0x%x DONE\n",
           mode, loops, value);
    return 0;
}
