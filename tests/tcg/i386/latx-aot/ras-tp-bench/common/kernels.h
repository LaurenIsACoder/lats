/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef KERNELS_H
#define KERNELS_H
#include <stdint.h>
#include <stddef.h>
typedef uint64_t (*kernel_fn)(uint64_t, uint64_t);
struct kernel { const char *name; kernel_fn fn; };
extern const struct kernel kernels[];
extern const size_t kernel_count;
void prepare_data(void);
#endif
