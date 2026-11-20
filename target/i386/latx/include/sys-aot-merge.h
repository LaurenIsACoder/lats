/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SYS_AOT_MERGE_H
#define SYS_AOT_MERGE_H

#include <stdint.h>

uintptr_t find_other_file_page(uintptr_t file_ptr, uint64_t file_size,
        uint32_t h, void *guest_code);
/* Count on success; -1 locked, -2 failed, -3 needs offline TU worker. */
int aot_merge_all(const char *file_name, uint32_t aot_mode);
void aot_merger_start(void);
void aot_merger_stop(void);
void aot_merger_drain(void);
void aot_merger_shutdown_drain(void);
void aot_merger_request(uint32_t req);

#endif
