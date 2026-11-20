/*
 * Internal structs that QEMU exports to TCG
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef QEMU_TB_CONTEXT_H
#define QEMU_TB_CONTEXT_H

#include "qemu/thread.h"
#include "qemu/qht.h"

//#define CODE_GEN_HTABLE_BITS     15
#define CODE_GEN_HTABLE_BITS     22
#define CODE_GEN_HTABLE_SIZE     (1 << CODE_GEN_HTABLE_BITS)

typedef struct TBContext TBContext;

struct TBContext {

    struct qht htable[MTCC_NR];
    int n;

    /* statistics */
    unsigned tb_flush_count;
    unsigned tb_phys_invalidate_count;

    uint32_t tb_flush_count_mtcc[MTCC_NR];
    uint32_t tb_phys_invalidate_count_mtcc[MTCC_NR];
};

extern TBContext tb_ctx;

#if HOST_LONG_BITS == 32
#define QHT_BUCKET_ENTRIES 6
#else /* 64-bit */
#define QHT_BUCKET_ENTRIES 4
#endif

#include "latx/include/latx-shadow-htable.h"

#endif
