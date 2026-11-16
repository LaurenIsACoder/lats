/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SYS_AOT_TU_CODEC_H
#define SYS_AOT_TU_CODEC_H

#include "sys-aot-format.h"
#include <stdio.h>

void aot_encode_block(aot_block *out, TranslationBlock *tb);
bool aot_encode_tu(FILE *file, TranslationBlock **tbs, int count,
                   const aot_tu_page *pages, uint32_t page_count,
                   int *size, uint64_t *id);
/* Fresh ordinary fragments only; retains all physical members and code. */
GBytes *aot_pack_function_tus(const GPtrArray *fragments, uint64_t entry);

#endif
