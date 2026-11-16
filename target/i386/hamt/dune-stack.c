/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "qemu/osdep.h"

#include "dune-stack.h"
#include "dune-info.h"
#include "dune-mmap.h"

void switch_host(struct dune_kvm_cpu *cpu)
{
    void *host_stack = mmap_n_pages(4);
    switch_stack(cpu, (uint64_t)host_stack + (PAGESIZE * 4));
}

struct dune_procmap_entry {
    unsigned long begin, end;
    long int offset;
    bool r, w, x, p;
    char *path;
    int type;
};

static uint64_t dune_expand_stack_getrlimit(void)
{
    struct rlimit rlim;
    if (getrlimit(RLIMIT_STACK, &rlim) == -1) {
        dune_die("get rlimit failed");
    }
    return rlim.rlim_cur;
}

static uint64_t dune_expand_stack_get_stack_top(void)
{
    struct dune_procmap_entry e;

    char line[512];
    char path[256];
    unsigned int dev1, dev2, inode;
    char read, write, execute, private;

    FILE *map = fopen("/proc/self/maps", "r");
    if (map == NULL) {
        dune_die("Could not open /proc/self/maps!\n");
    }

    if (setvbuf(map, NULL, _IOFBF, 8192)) {
        dune_die("setvbuf");
    }

    while (!feof(map)) {
        path[0] = '\0';
        if (fgets(line, 512, map) == NULL) break;
        sscanf((char *)&line, "%lx-%lx %c%c%c%c %lx %x:%x %d %s",
               &e.begin, &e.end, &read, &write, &execute, &private,
               &e.offset, &dev1, &dev2, &inode, path);
        if (strncmp(path, "[stack", 6) == 0) {
            return e.end;
        }
    }

    if (fclose(map)) {
        dune_die("close file");
    }

    dune_die("stack entry not found in /proc/self/maps");
    return 0;
}

void dune_expand_stack(void)
{
    uint64_t limit = dune_expand_stack_getrlimit();
    uint64_t top = dune_expand_stack_get_stack_top();
#ifdef CONFIG_LATX_PRINT
    dune_info("top = %llx, limit = %llx", top, limit);
#endif
    *(int *)(top - limit) = 0;
}
