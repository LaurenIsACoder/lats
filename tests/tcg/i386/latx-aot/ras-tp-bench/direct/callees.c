/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdint.h>
#define FN(N) __attribute__((noinline)) uint64_t callee##N(uint64_t x) { return (x ^ (UINT64_C(0x9e3779b97f4a7c15) + N)) + (x << 1); }
FN(0) FN(1) FN(2) FN(3)
