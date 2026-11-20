/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_VA_H_
#define _HAMT_VA_H_

extern int hamt_va;
static inline int is_host_kernel_48_va(void) {
    return ((uint64_t)(&hamt_va) >> 40);
}

#define HAMT_7F 0x7f0000000000

#endif
