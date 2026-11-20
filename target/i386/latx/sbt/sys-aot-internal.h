/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SYS_AOT_INTERNAL_H
#define SYS_AOT_INTERNAL_H

#include "sys-aot.h"
#include "qemu/rcu.h"

typedef struct AOTPublishedFile {
    struct rcu_head rcu;
    void *tb_ptr;
    uint64_t size;
    uint64_t *hash_table;
    uint32_t hash_size;
    uint32_t generation;
} AOTPublishedFile;

/* Cache file mappings, writer state and hash tables shared inside AOT SBT. */
extern int out_tb_file_control;
extern FILE *out_tb_file;
extern char *out_tb_filename;
extern char *hash_file_name;
extern char *aot_file_path;
extern int in_tb_hash_file;
extern const char *in_tb_hash_filename;
extern uint64_t *in_tb_hash_ptr;

extern AOTPublishedFile *core_aot_file, *user_aot_file;
extern __thread uint32_t aot_hash_size;
extern __thread uint32_t aot_file_generation;

extern __thread void *in_tb_ptr;
extern __thread uint64_t *hash_table;

AOTPublishedFile *load_aot_file(const char *file_name, uint32_t aot_mode);

#endif
