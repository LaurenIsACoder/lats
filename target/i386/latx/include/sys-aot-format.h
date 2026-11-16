/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SYS_AOT_FORMAT_H
#define SYS_AOT_FORMAT_H

#include "target/i386/latx/include/latx-config.h"
#include "exec/exec-all.h"
#include "latx-version.h"

/* AOT on-disk format shared by load, save, merge and verify code. */
#define HASH_MAP_BITS 21
#define HASH_MAP_SIZE (1 << HASH_MAP_BITS)
#define HASH_BUCKET_DEPTH 16
#define HASH_TABLE_SIZE sizeof(int64_t) * HASH_MAP_SIZE * HASH_BUCKET_DEPTH

#define PAGE_NO_LOAD 0x01
#define PAGE_NO_FIND 0x02
#define PAGE_LOADED  0x04

#include "lats/aot-format.h"

#ifdef CONFIG_LATX_DEBUG
#define AOT_VERSION "Version: " LATX_VERSION AOT_FORMAT_VERSION "-debug"
#else
#define AOT_VERSION "Version: " LATX_VERSION AOT_FORMAT_VERSION "-release"
#endif


aot_tail *get_aot_tail(int fd);
bool test_integrity(int fd, uint32_t aot_mode);
bool validate_aot_layout(const void *mem_ptr, uint64_t file_size);
bool save_aot_tail(int page_num, int tb_num, uint32_t aot_mode);
uint32_t get_page_hash(uint32_t *guest_code);
uint64_t get_page_fingerprint(const void *guest_code);

#endif
