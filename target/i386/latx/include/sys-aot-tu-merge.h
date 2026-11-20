/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SYS_AOT_TU_MERGE_H
#define SYS_AOT_TU_MERGE_H

#include "sys-aot-format.h"
#include <stdio.h>

typedef struct AOTFileCodePage {
    uint64_t pc;
    const uint8_t *bytes;
} AOTFileCodePage;

/*
 * Bound only while translating a compatible group in an offline process.
 * Pages are sorted by PC and each guest page has exactly one byte source.
 */
typedef struct AOTFileCodeView {
    const AOTFileCodePage *pages;
    size_t count;
    bool failed;
} AOTFileCodeView;

extern __thread AOTFileCodeView *aot_file_code_view;
extern __thread CPUState *aot_tu_merge_cpu;
const uint8_t *aot_file_code_page(uint64_t pc);

/* Inputs have passed validate_aot_layout(). Writes pages, index and tail. */
bool aot_merge_tu_files(const void *add, uint64_t add_size,
                       const void *base, uint64_t base_size, FILE *out,
                       uint32_t mode, int *pages, int *tbs);

/* Save owns the isolated CPU and scratch regions used by the merge worker. */
void aot_prepare_merge_translation(void);
void aot_reset_merge_translation(void);
void aot_request_tu_merge(bool merge);
bool aot_tu_merge_running(void);

#endif
