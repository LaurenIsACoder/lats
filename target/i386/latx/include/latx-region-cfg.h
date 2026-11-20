/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_REGION_CFG_H_
#define _LATX_REGION_CFG_H_

int latx_region_n_parts(void);

int latx_multi_region_n_parts(int rid);
size_t latx_multi_region_size(size_t tbsize, int rid);

#endif
