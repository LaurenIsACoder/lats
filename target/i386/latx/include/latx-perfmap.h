/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_PERFMAP_H_
#define _LATX_PERFMAP_H_

void latx_perfmap_insert(void *start, long len, const char *name);
void latx_perfmap_flush(void);

#endif
