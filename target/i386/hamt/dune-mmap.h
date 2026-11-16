/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _DUNE_MMAP_H_
#define _DUNE_MMAP_H_

#include <sys/mman.h>
#include "dune-arch.h"
#include "dune-info.h"

static inline void *mmap_pages(int num)
{
    void *addr = mmap(NULL, PAGESIZE * num,
            PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
            -1, 0);
    if (addr == NULL) {
        dune_die("mmap_one_page");
    }
    return addr;
}

static inline void *mmap_one_page(void)
{
    return mmap_pages(1);
}

static inline void *mmap_n_pages(int n)
{
    return mmap_pages(n);
}

#endif
