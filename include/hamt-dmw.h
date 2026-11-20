/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_DMW_H_
#define _HAMT_DMW_H_

#define DMW_VAL     ((uint64_t)0x9 << 60)
#define DMW_MASK    ((uint64_t)0xf << 60)

#define DMW_IN(p)   ((uint64_t)(p) |  (DMW_VAL))
#define DMW_OUT(p)  ((uint64_t)(p) & ~(DMW_MASK))

#endif
