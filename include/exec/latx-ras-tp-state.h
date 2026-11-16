/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef EXEC_LATX_RAS_TP_STATE_H
#define EXEC_LATX_RAS_TP_STATE_H

#include "qemu/typedefs.h"
#include <stdint.h>
#include <stdbool.h>

#define LATX_RAS_TP_SIZE 512

typedef struct LatxRasTPEntry {
    uint64_t pc;
    const void *ptr;
} LatxRasTPEntry;

/* Prefix matches Soft_RAS's cold top/reset/patch bookkeeping. */
typedef struct LatxRasTPState {
    uint32_t reset_pending;
    uint32_t top;
    TranslationBlock *patching_tb;
    LatxRasTPEntry sentinel;
    LatxRasTPEntry stack[LATX_RAS_TP_SIZE];
    uint64_t native_tp;
    uint32_t mtcc_id;
    bool initialized;
    bool link_queued;
    uint64_t initial_links, queued_links, repaired_links, expired_links;
} LatxRasTPState;

#endif
