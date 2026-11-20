/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_CPU_H_
#define _HAMT_CPU_H_

#define HAMT_CPU_MAX    16

extern int hamt_cpus_num;

typedef struct hamt_cpu_base_def {
    uint64_t data;
    uint64_t code;
    uint64_t err_data;
    uint64_t ipi_data;
    uint64_t int_data;
    uint64_t ptwrcd;
} hamt_cpu_base_def;

extern hamt_cpu_base_def hamt_cpu_base[];

#endif
