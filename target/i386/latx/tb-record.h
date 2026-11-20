/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef TB_RECORD_H
#define TB_RECORD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB_RECORD_MAGIC     0x54425243U
#define TB_RECORD_VERSION   1U
#define TB_RECORD_BUF_MAX   4096

/*
 * On-disk record header. Variable-length ir1_data and ir2_data follow.
 *
 * File layout:
 *   [magic 4B][version 4B][record_count 4B][reserved 4B]
 *   [TBRecordHeader][ir1_data...][ir2_data...]  (repeated per record)
 */
typedef struct TBRecordHeader {
    uint64_t pc;
    uint32_t cs_base;
    uint32_t flags;
    uint32_t cflags;
    uint64_t page_addr0;
    uint64_t page_addr1;
    uint16_t size;
    uint16_t icount;
    uint32_t ir1_len;
    uint32_t ir2_len;
} __attribute__((packed)) TBRecordHeader;

typedef struct TBRecordEntry {
    TBRecordHeader hdr;
    const uint8_t  *ir1_data;
    const uint32_t *ir2_data;
} TBRecordEntry;

int tb_record_init(const char *output_path);

int tb_record_tb(void *tb,
                 const uint8_t *ir1_data, uint32_t ir1_len,
                 const uint32_t *ir2_data, uint32_t ir2_len);

int tb_record_flush(void);

int tb_record_fini(void);

uint64_t tb_record_count(void);

#ifdef __cplusplus
}
#endif

#endif
