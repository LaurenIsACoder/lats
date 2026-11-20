/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SYS_AOT_COMMON_H
#define SYS_AOT_COMMON_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct AOTFileWriter {
    FILE *file;
    char final_name[PATH_MAX];
    char tmp_name[PATH_MAX];
    char *lock_name;
    int lockfd;
} AOTFileWriter;

char *gen_file_lock(const char *file_name);
void aot_generate_new_filename(const char *file_path, const char *filename,
        char *new_name, size_t size);
void aot_file_writer_init(AOTFileWriter *writer, const char *file_path,
        const char *file_name);
FILE *aot_file_writer_open(AOTFileWriter *writer);
void aot_file_writer_abort(AOTFileWriter *writer);
bool aot_file_writer_commit(AOTFileWriter *writer);
void *load_file_to_mem(char *file_name, uint64_t *size, uint32_t aot_mode);
#ifdef CONFIG_SOFTMMU
bool aot_host_pc_in_main_ram(void *host_pc);
#else
static inline bool aot_host_pc_in_main_ram(void *host_pc)
{
    /*
     * AOT is system-mode-only. User mode has no RAMBlock/main-RAM model;
     * reject these paths instead of reaching system-only page caches.
     */
    return false;
}
#endif

#endif
