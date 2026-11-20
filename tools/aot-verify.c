/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * aot-verify.c — Standalone offline AOT verification tool for LATX binary translation system.
 *
 * Reads a TB record file (binary format from tb-record module) and one or two
 * AOT files (sys-aot-core / sys-aot-user), then compares them to verify AOT
 * correctness.
 *
 * Usage:
 *   aot-verify <tb-record-file> <aot-core-file> [aot-user-file] [--report output.txt]
 *
 * Compile:
 *   gcc -O2 -o aot-verify aot-verify.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>

/* ============================================================
 * Constants
 * ============================================================ */

#define TB_RECORD_MAGIC   0x54425243U  /* "TBRC" */
#define AOT_PAGE_MAGIC    0x66666666U
#define AOT_MERGED_MAGIC  0x77777777U
#define AOT_TB_MAGIC      0xfefefefeU

#define MAX_TB_RECORDS    65536
#define MAX_AOT_TBS       65536
#define MAX_MISMATCHES    4096
#define MAX_UNMATCHED     4096

/* ============================================================
 * TB Record structures (standalone, no QEMU headers)
 * ============================================================ */

/* TB record file header:
 * [magic 4B][version 4B][record_count 4B][reserved 4B]
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t record_count;
    uint32_t reserved;
} tb_record_header;

/* TB record entry (variable length):
 * [pc 8B][cs_base 4B][flags 4B][cflags 4B]
 * [page_addr0 8B][page_addr1 8B][size 2B][icount 2B]
 * [ir1_len 4B][ir2_len 4B]
 * [ir1_data ...][ir2_data ...]
 */
typedef struct {
    uint64_t pc;
    uint32_t cs_base;
    uint32_t flags;
    uint32_t cflags;
    uint64_t page_addr[2];
    uint16_t size;
    uint16_t icount;
    uint32_t ir1_len;
    uint32_t ir2_len;
    /* ir1_data and ir2_data follow in file, stored separately */
    uint8_t *ir1_data;
    uint8_t *ir2_data;
} tb_record;

/* ============================================================
 * AOT file structures (mirrors sys-aot.h, standalone)
 * ============================================================ */

/* page_head: located at the start of each page region in the AOT file */
typedef struct {
    uint32_t page_magic;
    uint32_t page_hash;
    uint32_t tb_count;
    int32_t  curr_page_sum_size;
} aot_page_head;

/* aot_block: metadata for each translated block inside the AOT file.
 * This is a simplified version that captures only the fields needed
 * for verification.  The on-disk layout matches the one produced by
 * tb_to_aot_block() in sys-aot.c.
 *
 * NOTE: CONFIG_LATX_TU is assumed NOT defined for this standalone tool.
 * The struct size must match what was written by fwrite(&aot_tb, sizeof(aot_block), ...).
 * We read the raw bytes and extract fields by offset to avoid ABI issues.
 */
typedef struct {
    uint64_t  pc;
    uint64_t  page_addr[2];
    uint64_t  next_pc[2];
    uint64_t  jmp_target_addr[2];
    union {
        uint64_t search_size;
        void    *search_ptr;
    };
    uint64_t  tc_offset_in_tu;       /* struct aot_tc { size_t offset_in_tu; } */
    union {
        uintptr_t x86_next_pc;
        uint32_t  tu_link_ins;
    };
    union {
        uintptr_t x86_target_pc;
        uint32_t  tu_unlink_stub_offset;
    };
    uint32_t  cs_base;
    uint32_t  misc_flags;
    uint32_t  flags;
    uint32_t  cflags;
    uint32_t  ir2_num;
    int32_t   inst_ptr_offset;
    int32_t   label_pos_nocheck;
    int32_t   tu_jmp[2];
    int32_t   rel_start;
    int32_t   rel_end;
    /* Without CONFIG_LATX_TU, the following fields are absent */
} aot_block;

/* aot_rel: relocation entry */
typedef struct {
    int32_t   kind;
    uint32_t  tc_offset;
    uint32_t  rel_slots_num;
    uint64_t  extra_addend;
    int32_t   next_entry_id;
} aot_rel;

/* aot_tail: at the very end of the AOT file */
typedef struct {
    uint64_t flags;
    int32_t  page_num;
    int32_t  tb_num;
    /* helper_addr array and aot_version string follow */
} aot_tail;

/* page_info_t: page info array entry */
typedef struct {
    uint32_t page_hash;
    uint64_t file_offset;
} aot_page_info;

/* page_info_array_t: page info array header */
typedef struct {
    uint32_t page_count;
    /* pages[] follows */
} aot_page_info_array;

/* ============================================================
 * Internal representation for comparison
 * ============================================================ */

/* Normalised TB entry used for matching between record and AOT */
typedef struct {
    uint64_t  pc;
    uint32_t  cs_base;
    uint32_t  flags;
    uint32_t  cflags;
    uint64_t  page_addr[2];
    uint16_t  size;
    uint16_t  icount;
    uint32_t  ir2_num;
    uint8_t  *ir2_data;
    uint32_t  ir2_len;
    uint8_t  *ir1_data;
    uint32_t  ir1_len;
    int       source;
    int       matched;
} tb_entry;

/* Mismatch detail */
typedef struct {
    uint64_t  pc;
    uint32_t  cs_base;
    uint32_t  flags;
    uint32_t  cflags;
    uint16_t  rec_size;
    uint16_t  aot_size;
    uint16_t  rec_icount;
    uint16_t  aot_icount;
    uint32_t  rec_ir2_num;
    uint32_t  aot_ir2_num;
    uint32_t  rec_ir2_len;
    uint32_t  aot_ir2_len;
    const uint8_t *rec_ir2;
    const uint8_t *aot_ir2;
    uint32_t  rec_ir1_len;
    uint32_t  aot_ir1_len;
    const uint8_t *rec_ir1;
    const uint8_t *aot_ir1;
    int         mismatch_type;
} mismatch_detail;

/* ============================================================
 * Global storage
 * ============================================================ */

static tb_record   g_records[MAX_TB_RECORDS];
static int         g_record_count = 0;

static tb_entry    g_tb_entries[MAX_TB_RECORDS + MAX_AOT_TBS];
static int         g_entry_count = 0;

static mismatch_detail g_mismatches[MAX_MISMATCHES];
static int         g_mismatch_count = 0;

static tb_entry    g_unmatched_rec[MAX_UNMATCHED];
static int         g_unmatched_rec_count = 0;

static tb_entry    g_unmatched_aot[MAX_UNMATCHED];
static int         g_unmatched_aot_count = 0;

static int         g_total_matched = 0;

/* ============================================================
 * Utility: read little-endian values (portable)
 * ============================================================ */

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p) {
    return (uint64_t)read_u32_le(p) | ((uint64_t)read_u32_le(p + 4) << 32);
}

static int32_t read_i32_le(const uint8_t *p) {
    return (int32_t)read_u32_le(p);
}

/* ============================================================
 * TB Record file parser
 * ============================================================ */

static int parse_tb_record_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open TB record file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    /* Read header */
    uint8_t hdr_buf[16];
    if (fread(hdr_buf, 1, 16, f) != 16) {
        fprintf(stderr, "Error: failed to read TB record header\n");
        fclose(f);
        return -1;
    }

    tb_record_header hdr;
    hdr.magic        = read_u32_le(hdr_buf);
    hdr.version      = read_u32_le(hdr_buf + 4);
    hdr.record_count = read_u32_le(hdr_buf + 8);
    hdr.reserved     = read_u32_le(hdr_buf + 12);

    if (hdr.magic != TB_RECORD_MAGIC) {
        fprintf(stderr, "Error: invalid TB record magic 0x%08x (expected 0x%08x)\n",
                hdr.magic, TB_RECORD_MAGIC);
        fclose(f);
        return -1;
    }

    if (hdr.record_count > MAX_TB_RECORDS) {
        fprintf(stderr, "Error: record count %u exceeds maximum %d\n",
                hdr.record_count, MAX_TB_RECORDS);
        fclose(f);
        return -1;
    }

    printf("TB Record: magic=0x%08x version=%u count=%u\n",
           hdr.magic, hdr.version, hdr.record_count);

    /* Read each record */
    for (uint32_t i = 0; i < hdr.record_count; i++) {
        uint8_t rec_hdr[48];
        if (fread(rec_hdr, 1, 48, f) != 48) {
            fprintf(stderr, "Error: failed to read record %u header\n", i);
            fclose(f);
            return -1;
        }

        tb_record *rec = &g_records[g_record_count];
        rec->pc          = read_u64_le(rec_hdr);
        rec->cs_base     = read_u32_le(rec_hdr + 8);
        rec->flags       = read_u32_le(rec_hdr + 12);
        rec->cflags      = read_u32_le(rec_hdr + 16);
        rec->page_addr[0] = read_u64_le(rec_hdr + 20);
        rec->page_addr[1] = read_u64_le(rec_hdr + 28);
        rec->size        = read_u16_le(rec_hdr + 36);
        rec->icount      = read_u16_le(rec_hdr + 38);
        rec->ir1_len     = read_u32_le(rec_hdr + 40);
        rec->ir2_len     = read_u32_le(rec_hdr + 44);

        /* Read IR1 data */
        if (rec->ir1_len > 0) {
            rec->ir1_data = (uint8_t *)malloc(rec->ir1_len);
            if (!rec->ir1_data) {
                fprintf(stderr, "Error: malloc failed for IR1 data of record %u\n", i);
                fclose(f);
                return -1;
            }
            if (fread(rec->ir1_data, 1, rec->ir1_len, f) != rec->ir1_len) {
                fprintf(stderr, "Error: failed to read IR1 data of record %u\n", i);
                fclose(f);
                return -1;
            }
        } else {
            rec->ir1_data = NULL;
        }

        /* Read IR2 data */
        if (rec->ir2_len > 0) {
            rec->ir2_data = (uint8_t *)malloc(rec->ir2_len);
            if (!rec->ir2_data) {
                fprintf(stderr, "Error: malloc failed for IR2 data of record %u\n", i);
                fclose(f);
                return -1;
            }
            if (fread(rec->ir2_data, 1, rec->ir2_len, f) != rec->ir2_len) {
                fprintf(stderr, "Error: failed to read IR2 data of record %u\n", i);
                fclose(f);
                return -1;
            }
        } else {
            rec->ir2_data = NULL;
        }

        g_record_count++;
    }

    fclose(f);
    printf("TB Record: loaded %d records\n", g_record_count);
    return 0;
}

/* ============================================================
 * AOT file parser
 * ============================================================ */

/* Parse aot_block from raw bytes.  The on-disk struct is written by
 * fwrite(&aot_tb, sizeof(aot_block), ...) so we read field-by-field
 * using the known layout.  Without CONFIG_LATX_TU the struct ends
 * after rel_end.
 *
 * Layout (no TU):
 *   pc              0  8
 *   page_addr[2]    8  16
 *   next_pc[2]     24  16
 *   jmp_target[2]  40  16
 *   search_size    56  8
 *   tc (offset)    64  8  (size_t)
 *   x86_next_pc    72  8
 *   x86_target_pc  80  8
 *   cs_base        88  4
 *   misc_flags     92  4
 *   flags          96  4
 *   cflags        100  4
 *   ir2_num       104  4
 *   inst_ptr_off  108  4
 *   label_pos_nc  112  4
 *   tu_jmp[2]     116  8
 *   rel_start     124  4
 *   rel_end       128  4
 * Total: 132 bytes (without TU fields)
 */
#define AOT_BLOCK_SIZE_NO_TU  132

static void parse_aot_block_from_bytes(aot_block *blk, const uint8_t *p) {
    memset(blk, 0, sizeof(*blk));
    blk->pc              = read_u64_le(p);
    blk->page_addr[0]    = read_u64_le(p + 8);
    blk->page_addr[1]    = read_u64_le(p + 16);
    blk->next_pc[0]      = read_u64_le(p + 24);
    blk->next_pc[1]      = read_u64_le(p + 32);
    blk->jmp_target_addr[0] = read_u64_le(p + 40);
    blk->jmp_target_addr[1] = read_u64_le(p + 48);
    blk->search_size     = read_u64_le(p + 56);
    blk->tc_offset_in_tu = read_u64_le(p + 64);
    blk->x86_next_pc     = (uintptr_t)read_u64_le(p + 72);
    blk->x86_target_pc   = (uintptr_t)read_u64_le(p + 80);
    blk->cs_base         = read_u32_le(p + 88);
    blk->misc_flags      = read_u32_le(p + 92);
    blk->flags           = read_u32_le(p + 96);
    blk->cflags          = read_u32_le(p + 100);
    blk->ir2_num         = read_u32_le(p + 104);
    blk->inst_ptr_offset = read_i32_le(p + 108);
    blk->label_pos_nocheck = read_i32_le(p + 112);
    blk->tu_jmp[0]       = read_i32_le(p + 116);
    blk->tu_jmp[1]       = read_i32_le(p + 120);
    blk->rel_start       = read_i32_le(p + 124);
    blk->rel_end         = read_i32_le(p + 128);
}

/* Parse a single AOT file (core or user).
 * File layout:
 *   [page_head + guest_code + TB blocks + ...] * N pages
 *   [page_info_array]
 *   [aot_tail]
 *
 * Each TB block:
 *   [tb_magic 4B][sum_size 4B][all_tb_size 4B][rel_count 4B]
 *   [aot_block (132B)]
 *   [ir2_code (all_tb_size - sizeof(aot_block))]
 *   [rel_table (rel_count * sizeof(aot_rel))]
 */
static int parse_aot_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open AOT file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Read entire file into memory */
    uint8_t *data = (uint8_t *)malloc(file_size);
    if (!data) {
        fprintf(stderr, "Error: cannot allocate %ld bytes for AOT file\n", file_size);
        fclose(f);
        return -1;
    }
    if (fread(data, 1, file_size, f) != (size_t)file_size) {
        fprintf(stderr, "Error: failed to read AOT file\n");
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    printf("AOT file '%s': size=%ld bytes\n", path, file_size);

    long offset = 0;
    int total_tbs = 0;

    while (offset < file_size) {
        /* Try to read page_head */
        if (offset + (long)sizeof(aot_page_head) > file_size) {
            /* Might be page_info_array or aot_tail at the end */
            break;
        }

        aot_page_head ph;
        ph.page_magic      = read_u32_le(data + offset);
        ph.page_hash       = read_u32_le(data + offset + 4);
        ph.tb_count        = read_u32_le(data + offset + 8);
        ph.curr_page_sum_size = read_i32_le(data + offset + 12);

        /* Check if this looks like a valid page head */
        if (ph.page_magic != AOT_PAGE_MAGIC && ph.page_magic != AOT_MERGED_MAGIC) {
            /* Not a page head — likely reached page_info_array or aot_tail */
            break;
        }

        if (ph.curr_page_sum_size <= 0 || offset + ph.curr_page_sum_size > file_size) {
            fprintf(stderr, "Warning: invalid page_sum_size %d at offset %ld, stopping\n",
                    ph.curr_page_sum_size, offset);
            break;
        }

        printf("  Page: magic=0x%08x hash=0x%08x tb_count=%u sum_size=%d\n",
               ph.page_magic, ph.page_hash, ph.tb_count, ph.curr_page_sum_size);

        long page_end = offset + ph.curr_page_sum_size;
        long tb_offset = offset + sizeof(aot_page_head) + 4096; /* skip page_head + guest_code */

        for (uint32_t t = 0; t < ph.tb_count; t++) {
            if (tb_offset + 16 > page_end) {
                fprintf(stderr, "Warning: TB %u extends beyond page end\n", t);
                break;
            }

            uint32_t tb_magic   = read_u32_le(data + tb_offset);
            uint32_t sum_size   = read_u32_le(data + tb_offset + 4);
            uint32_t all_tb_size = read_u32_le(data + tb_offset + 8);
            uint32_t rel_count  = read_u32_le(data + tb_offset + 12);
            (void)rel_count;

            if (tb_magic != AOT_TB_MAGIC) {
                fprintf(stderr, "Warning: invalid TB magic 0x%08x at offset %ld\n",
                        tb_magic, tb_offset);
                break;
            }

            long aot_block_off = tb_offset + 16;
            if (aot_block_off + AOT_BLOCK_SIZE_NO_TU > page_end) {
                fprintf(stderr, "Warning: aot_block extends beyond page\n");
                break;
            }

            aot_block blk;
            parse_aot_block_from_bytes(&blk, data + aot_block_off);

            /* IR2 code starts right after aot_block */
            long ir2_off = aot_block_off + AOT_BLOCK_SIZE_NO_TU;
            uint32_t ir2_len = all_tb_size - AOT_BLOCK_SIZE_NO_TU;
            if (ir2_len > 0 && ir2_off + ir2_len > page_end) {
                fprintf(stderr, "Warning: IR2 data extends beyond page for PC=0x%" PRIx64 "\n",
                        blk.pc);
                ir2_len = 0;
            }

            uint8_t *guest_code = data + offset + sizeof(aot_page_head);
            uint32_t ir1_offset = (uint32_t)(blk.pc & 0xFFF);

            if (g_entry_count < MAX_TB_RECORDS + MAX_AOT_TBS) {
                tb_entry *e = &g_tb_entries[g_entry_count];
                e->pc          = blk.pc;
                e->cs_base     = blk.cs_base;
                e->flags       = blk.flags;
                e->cflags      = blk.cflags;
                e->page_addr[0] = blk.page_addr[0];
                e->page_addr[1] = blk.page_addr[1];
                e->size        = 0;
                e->icount      = 0;
                e->ir2_num     = blk.ir2_num;
                e->ir2_data    = (ir2_len > 0) ? (data + ir2_off) : NULL;
                e->ir2_len     = ir2_len;
                e->ir1_data    = guest_code + ir1_offset;
                e->ir1_len     = 0;
                e->source      = 1;
                e->matched     = 0;
                g_entry_count++;
            }

            total_tbs++;

            /* Advance to next TB block */
            tb_offset += 16 + sum_size;
        }

        offset = page_end;
    }

    printf("AOT file '%s': parsed %d TBs\n", path, total_tbs);

    static uint8_t *aot_buffers[16];
    static int aot_buf_count = 0;
    if (aot_buf_count < 16) {
        aot_buffers[aot_buf_count++] = data;
    } else {
        free(data);
    }
    (void)aot_buffers;

    return 0;
}

/* ============================================================
 * Comparison logic
 * ============================================================ */

/* Hash key for matching: (pc, cs_base, flags, cflags) */
static int tb_key_match(const tb_entry *a, const tb_entry *b) {
    return a->pc == b->pc &&
           a->cs_base == b->cs_base &&
           a->flags == b->flags &&
           a->cflags == b->cflags;
}

static void compare_entries(void) {
    printf("\nComparing TB records with AOT TBs...\n");

    /* For each record entry, try to find a matching AOT entry */
    for (int i = 0; i < g_record_count; i++) {
        tb_record *rec = &g_records[i];
        int found = 0;

        for (int j = 0; j < g_entry_count; j++) {
            tb_entry *aot = &g_tb_entries[j];
            if (aot->source != 1) continue; /* Only AOT entries */
            if (aot->matched) continue;     /* Already matched */

            /* Build a temporary tb_entry from record for comparison */
            tb_entry rec_entry;
            rec_entry.pc       = rec->pc;
            rec_entry.cs_base  = rec->cs_base;
            rec_entry.flags    = rec->flags;
            rec_entry.cflags   = rec->cflags;
            rec_entry.page_addr[0] = rec->page_addr[0];
            rec_entry.page_addr[1] = rec->page_addr[1];
            rec_entry.size     = rec->size;
            rec_entry.icount   = rec->icount;
            rec_entry.ir2_num  = rec->ir2_len / 4;
            rec_entry.ir2_data = rec->ir2_data;
            rec_entry.ir2_len  = rec->ir2_len;
            rec_entry.ir1_data = rec->ir1_data;
            rec_entry.ir1_len  = rec->ir1_len;
            rec_entry.source   = 0;
            rec_entry.matched  = 0;

            if (!tb_key_match(&rec_entry, aot)) continue;

            aot->matched = 1;
            found = 1;
            g_total_matched++;

            int mismatch_type = 0;

            if (rec_entry.ir2_num != aot->ir2_num) {
                mismatch_type |= 1;
            }

            uint32_t cmp_len = rec_entry.ir2_len < aot->ir2_len ?
                               rec_entry.ir2_len : aot->ir2_len;
            if (rec_entry.ir2_len != aot->ir2_len ||
                memcmp(rec_entry.ir2_data, aot->ir2_data, cmp_len) != 0) {
                mismatch_type |= 2;
            }

            if (rec_entry.ir1_len > 0 && aot->ir1_data) {
                if (memcmp(rec_entry.ir1_data, aot->ir1_data, rec_entry.ir1_len) != 0) {
                    mismatch_type |= 4;
                }
            }

            if (mismatch_type) {
                if (g_mismatch_count < MAX_MISMATCHES) {
                    mismatch_detail *md = &g_mismatches[g_mismatch_count];
                    md->pc          = rec->pc;
                    md->cs_base     = rec->cs_base;
                    md->flags       = rec->flags;
                    md->cflags      = rec->cflags;
                    md->rec_size    = rec->size;
                    md->aot_size    = 0;
                    md->rec_icount  = rec->icount;
                    md->aot_icount  = 0;
                    md->rec_ir2_num = rec_entry.ir2_num;
                    md->aot_ir2_num = aot->ir2_num;
                    md->rec_ir2_len = rec_entry.ir2_len;
                    md->aot_ir2_len = aot->ir2_len;
                    md->rec_ir2     = rec_entry.ir2_data;
                    md->aot_ir2     = aot->ir2_data;
                    md->rec_ir1_len = rec_entry.ir1_len;
                    md->aot_ir1_len = rec_entry.ir1_len;
                    md->rec_ir1     = rec_entry.ir1_data;
                    md->aot_ir1     = aot->ir1_data;
                    md->mismatch_type = mismatch_type;
                    g_mismatch_count++;
                }
            }
            break;
        }

        if (!found) {
            /* Record entry not found in AOT */
            if (g_unmatched_rec_count < MAX_UNMATCHED) {
                tb_entry *u = &g_unmatched_rec[g_unmatched_rec_count];
                u->pc       = rec->pc;
                u->cs_base  = rec->cs_base;
                u->flags    = rec->flags;
                u->cflags   = rec->cflags;
                u->source   = 0;
                g_unmatched_rec_count++;
            }
        }
    }

    /* Find AOT entries not in records */
    for (int j = 0; j < g_entry_count; j++) {
        tb_entry *aot = &g_tb_entries[j];
        if (aot->source != 1) continue;
        if (aot->matched) continue;

        if (g_unmatched_aot_count < MAX_UNMATCHED) {
            tb_entry *u = &g_unmatched_aot[g_unmatched_aot_count];
            u->pc       = aot->pc;
            u->cs_base  = aot->cs_base;
            u->flags    = aot->flags;
            u->cflags   = aot->cflags;
            u->source   = 1;
            g_unmatched_aot_count++;
        }
    }
}

/* ============================================================
 * LoongArch IR2 disassembler
 * ============================================================ */

typedef struct {
    uint16_t op15;
    const char *name;
} ir2_op_entry;

static const ir2_op_entry ir2_ops[] = {
    {0x020, "add.w"},   {0x021, "add.d"},   {0x022, "sub.w"},   {0x023, "sub.d"},
    {0x024, "slt"},     {0x025, "sltu"},    {0x026, "masknez"}, {0x027, "maskeqz"},
    {0x028, "nor"},     {0x029, "and"},     {0x02a, "or"},      {0x02b, "xor"},
    {0x02c, "orn"},     {0x02d, "andn"},    {0x02e, "sll.w"},   {0x02f, "sll.d"},
    {0x030, "srl.w"},   {0x031, "srl.d"},   {0x032, "sra.w"},   {0x033, "sra.d"},
    {0x034, "rotr.w"},  {0x035, "rotr.d"},
    {0x038, "mul.w"},   {0x039, "mulh.w"},  {0x03a, "mulh.wu"}, {0x03b, "mul.d"},
    {0x03c, "mulh.d"},  {0x03d, "mulh.du"},
    {0x040, "div.w"},   {0x041, "mod.w"},   {0x042, "div.wu"},  {0x043, "mod.wu"},
    {0x044, "div.d"},   {0x045, "mod.d"},   {0x046, "div.du"},  {0x047, "mod.du"},
    {0x048, "break"},   {0x049, "dbcl"},    {0x04a, "syscall"},
    {0x060, "crc.w.b.w"},{0x061,"crc.w.h.w"},{0x062,"crc.w.w.w"},{0x063,"crc.w.d.w"},
    {0x064, "crcc.w.b.w"},{0x065,"crcc.w.h.w"},{0x066,"crcc.w.w.w"},{0x067,"crcc.w.d.w"},
    {0x080, "addu16i.d"},
    {0x088, "alsl.w"},  {0x089, "alsl.wu"}, {0x08a, "alsl.d"},
    {0x090, "bytepick.w"},{0x091,"bytepick.d"},
    {0x094, "fadd.s"},  {0x095, "fadd.d"},  {0x096, "fsub.s"},  {0x097, "fsub.d"},
    {0x098, "fmul.s"},  {0x099, "fmul.d"},  {0x09a, "fdiv.s"},  {0x09b, "fdiv.d"},
    {0x09c, "fmax.s"},  {0x09d, "fmax.d"},  {0x09e, "fmin.s"},  {0x09f, "fmin.d"},
    {0x0a0, "fmaxa.s"}, {0x0a1, "fmaxa.d"}, {0x0a2, "fmina.s"}, {0x0a3, "fmina.d"},
    {0x0a4, "fscaleb.s"},{0x0a5,"fscaleb.d"},{0x0a6,"fcopysign.s"},{0x0a7,"fcopysign.d"},
    {0x0c8, "fmadd.s"}, {0x0c9, "fmadd.d"}, {0x0ca, "fmsub.s"}, {0x0cb, "fmsub.d"},
    {0x0cc, "fnmadd.s"},{0x0cd, "fnmadd.d"},{0x0ce, "fnmsub.s"},{0x0cf, "fnmsub.d"},
    {0x0d8, "fcmp.cmp.s"},{0x0d9,"fcmp.cmp.d"},
    {0x0a8, "slti"},    {0x0a9, "sltui"},   {0x0aa, "addi.w"},  {0x0ab, "addi.d"},
    {0x0ac, "lu52i.d"}, {0x0ad, "andi"},     {0x0ae, "ori"},     {0x0af, "xori"},
    {0x0b8, "slli.w"},  {0x0b9, "slli.d"},  {0x0ba, "srli.w"},  {0x0bb, "srli.d"},
    {0x0bc, "srai.w"},  {0x0bd, "srai.d"},  {0x0be, "rotri.w"},{0x0bf, "rotri.d"},
    {0x0a0, "ldl.d"},   {0x0a1, "ldr.d"},
    {0x0a2, "ldl.w"},   {0x0a3, "ldr.w"},
    {0x0a4, "stl.d"},   {0x0a5, "str.d"},
    {0x0a6, "stl.w"},   {0x0a7, "str.w"},
    {0x0a2, "ll.w"},    {0x0a3, "ll.d"},
    {0x0a6, "sc.w"},    {0x0a7, "sc.d"},
    {0x140, "ld.b"},    {0x141, "ld.h"},    {0x142, "ld.w"},    {0x143, "ld.d"},
    {0x144, "st.b"},    {0x145, "st.h"},    {0x146, "st.w"},    {0x147, "st.d"},
    {0x148, "ld.bu"},   {0x149, "ld.hu"},   {0x14a, "ld.wu"},
    {0x14b, "preld"},
    {0x150, "fld.s"},   {0x151, "fst.s"},   {0x152, "fld.d"},   {0x153, "fst.d"},
    {0x160, "beq"},     {0x161, "bne"},     {0x162, "blt"},     {0x163, "bge"},
    {0x164, "bltu"},    {0x165, "bgeu"},
    {0x166, "beqz"},    {0x167, "bnez"},
    {0x180, "lu12i.w"}, {0x181, "lu32i.d"},
    {0x182, "pcaddi"},  {0x183, "pcalau12i"},{0x184, "pcaddu12i"},{0x185, "pcaddu18i"},
    {0x186, "addu12i.w"},
    {0x1c0, "jirl"},
    {0x200, "ext.w.b"}, {0x201, "ext.w.h"}, {0x202, "clo.w"},   {0x203, "clz.w"},
    {0x204, "cto.w"},   {0x205, "ctz.w"},   {0x206, "clo.d"},   {0x207, "clz.d"},
    {0x208, "cto.d"},   {0x209, "ctz.d"},   {0x20a, "revb.2h"}, {0x20b, "revb.4h"},
    {0x20c, "revb.2w"}, {0x20d, "revb.d"},  {0x20e, "revh.2w"}, {0x20f, "revh.d"},
    {0x210, "bitrev.4b"},{0x211,"bitrev.8b"},{0x212,"bitrev.w"}, {0x213,"bitrev.d"},
    {0x214, "bstrins.w"},{0x215,"bstrpick.w"},{0x216,"bstrins.d"},{0x217,"bstrpick.d"},
    {0x240, "fcvt.s.d"},{0x241, "fcvt.d.s"},
    {0x242, "ftintrm.w.s"},{0x243,"ftintrm.w.d"},{0x244,"ftintrm.l.s"},{0x245,"ftintrm.l.d"},
    {0x248, "ftintrp.w.s"},{0x249,"ftintrp.w.d"},{0x24a,"ftintrp.l.s"},{0x24b,"ftintrp.l.d"},
    {0x24c, "ftintrz.w.s"},{0x24d,"ftintrz.w.d"},{0x24e,"ftintrz.l.s"},{0x24f,"ftintrz.l.d"},
    {0x250, "ftintrne.w.s"},{0x251,"ftintrne.w.d"},{0x252,"ftintrne.l.s"},{0x253,"ftintrne.l.d"},
    {0x254, "ftint.w.s"},{0x255,"ftint.w.d"},{0x256,"ftint.l.s"},{0x257,"ftint.l.d"},
    {0x258, "ffint.s.w"},{0x259,"ffint.s.l"},{0x25a,"ffint.d.w"},{0x25b,"ffint.d.l"},
    {0x25c, "frint.s"},  {0x25d, "frint.d"},
    {0x260, "fcmp.caf.s"},{0x261,"fcmp.caf.d"},
    {0x262, "fcmp.cun.s"},{0x263,"fcmp.cun.d"},
    {0x264, "fcmp.ceq.s"},{0x265,"fcmp.ceq.d"},
    {0x266, "fcmp.cueq.s"},{0x267,"fcmp.cueq.d"},
    {0x268, "fcmp.clt.s"},{0x269,"fcmp.clt.d"},
    {0x26a, "fcmp.cult.s"},{0x26b,"fcmp.cult.d"},
    {0x26c, "fcmp.cle.s"},{0x26d,"fcmp.cle.d"},
    {0x26e, "fcmp.cule.s"},{0x26f,"fcmp.cule.d"},
    {0x270, "fcmp.cne.s"},{0x271,"fcmp.cne.d"},
    {0x272, "fcmp.cor.s"},{0x273,"fcmp.cor.d"},
    {0x274, "fcmp.cune.s"},{0x275,"fcmp.cune.d"},
    {0x278, "fsel"},
    {0x280, "amswap.w"},{0x281,"amswap.d"},{0x282,"amadd.w"},{0x283,"amadd.d"},
    {0x284, "amand.w"}, {0x285,"amand.d"}, {0x286,"amor.w"},  {0x287,"amor.d"},
    {0x288, "amxor.w"}, {0x289,"amxor.d"}, {0x28a,"ammax.w"},{0x28b,"ammax.d"},
    {0x28c, "ammin.w"}, {0x28d,"ammin.d"}, {0x28e,"ammax.wu"},{0x28f,"ammax.du"},
    {0x290, "ammin.wu"},{0x291,"ammin.du"},{0x292,"amswap_db.w"},{0x293,"amswap_db.d"},
    {0x294, "amadd_db.w"},{0x295,"amadd_db.d"},{0x296,"amand_db.w"},{0x297,"amand_db.d"},
    {0x298, "amor_db.w"},{0x299,"amor_db.d"},{0x29a,"amxor_db.w"},{0x29b,"amxor_db.d"},
    {0x29c, "ammax_db.w"},{0x29d,"ammax_db.d"},{0x29e,"ammin_db.w"},{0x29f,"ammin_db.d"},
    {0x2a0, "ammax_db.wu"},{0x2a1,"ammax_db.du"},{0x2a2,"ammin_db.wu"},{0x2a3,"ammin_db.du"},
    {0x2c0, "dbar"},    {0x2c1, "ibar"},
    {0x2c2, "fence"},   {0x2c3, "fencei"},
    {0x2c4, "idle"},    {0x2c5, "invtlb"},
    {0x340, "cacop"},   {0x341, "lddir"},   {0x342, "ldpte"},
    {0x348, "csrrd"},   {0x349, "csrwr"},   {0x34a, "csrxchg"},
    {0x350, "iocsrrd.b"},{0x351,"iocsrrd.h"},{0x352,"iocsrrd.w"},{0x353,"iocsrrd.d"},
    {0x354, "iocsrwr.b"},{0x355,"iocsrwr.h"},{0x356,"iocsrwr.w"},{0x357,"iocsrwr.d"},
    {0x358, "tlbrd"},   {0x359, "tlbwr"},   {0x35a, "tlbfill"}, {0x35b, "tlbflush"},
    {0x35c, "ertn"},

    {0, NULL}
};

static const char *ir2_lookup_mnemonic(uint32_t inst) {
    uint16_t op15 = (uint16_t)(inst >> 15);
    for (const ir2_op_entry *e = ir2_ops; e->name; e++) {
        if (e->op15 == op15) return e->name;
    }
    return NULL;
}

static int ir2_disasm(char *buf, int bufsize, uint32_t inst) {
    uint8_t rd  = (inst >> 0)  & 0x1f;
    uint8_t rj  = (inst >> 5)  & 0x1f;
    uint8_t rk  = (inst >> 10) & 0x1f;
    int32_t si12 = (int32_t)(inst >> 10) & 0xfff;
    if (si12 & 0x800) si12 |= ~0xfff;
    int32_t si16 = (int32_t)(inst >> 10) & 0xffff;
    if (si16 & 0x8000) si16 |= ~0xffff;
    int32_t si20 = (int32_t)(inst >> 5) & 0xfffff;
    if (si20 & 0x80000) si20 |= ~0xfffff;

    const char *mn = ir2_lookup_mnemonic(inst);
    if (!mn) {
        return snprintf(buf, bufsize, "%08x", inst);
    }

    if (strcmp(mn, "jirl") == 0)
        return snprintf(buf, bufsize, "%-8s r%d, r%d, %d", mn, rd, rj, si16);

    if (strcmp(mn, "beq") == 0 || strcmp(mn, "bne") == 0 ||
        strcmp(mn, "blt") == 0 || strcmp(mn, "bge") == 0 ||
        strcmp(mn, "bltu") == 0 || strcmp(mn, "bgeu") == 0)
        return snprintf(buf, bufsize, "%-8s r%d, r%d, %d", mn, rd, rj, si16);

    if (strcmp(mn, "beqz") == 0 || strcmp(mn, "bnez") == 0)
        return snprintf(buf, bufsize, "%-8s r%d, %d", mn, rj, si16);

    if (strcmp(mn, "lu12i.w") == 0 || strcmp(mn, "lu32i.d") == 0 ||
        strcmp(mn, "pcaddi") == 0 || strcmp(mn, "pcalau12i") == 0 ||
        strcmp(mn, "pcaddu12i") == 0 || strcmp(mn, "pcaddu18i") == 0 ||
        strcmp(mn, "addu12i.w") == 0)
        return snprintf(buf, bufsize, "%-8s r%d, %d", mn, rd, si20);

    if (strncmp(mn, "ld.", 3) == 0 || strncmp(mn, "st.", 3) == 0 ||
        strncmp(mn, "fld.", 4) == 0 || strncmp(mn, "fst.", 4) == 0 ||
        strncmp(mn, "ldl.", 4) == 0 || strncmp(mn, "ldr.", 4) == 0 ||
        strncmp(mn, "stl.", 4) == 0 || strncmp(mn, "str.", 4) == 0 ||
        strncmp(mn, "ll.", 3) == 0 || strncmp(mn, "sc.", 3) == 0 ||
        strncmp(mn, "ldbu", 4) == 0 || strncmp(mn, "ldhu", 4) == 0 ||
        strncmp(mn, "ldwu", 4) == 0 || strcmp(mn, "preld") == 0)
        return snprintf(buf, bufsize, "%-8s r%d, r%d, %d", mn, rd, rj, si12);

    if (strcmp(mn, "slti") == 0 || strcmp(mn, "sltui") == 0 ||
        strcmp(mn, "addi.w") == 0 || strcmp(mn, "addi.d") == 0 ||
        strcmp(mn, "andi") == 0 || strcmp(mn, "ori") == 0 ||
        strcmp(mn, "xori") == 0 || strcmp(mn, "lu52i.d") == 0 ||
        strcmp(mn, "addu16i.d") == 0 || strcmp(mn, "preld") == 0)
        return snprintf(buf, bufsize, "%-8s r%d, r%d, %d", mn, rd, rj, si12);

    if (strcmp(mn, "slli.w") == 0 || strcmp(mn, "slli.d") == 0 ||
        strcmp(mn, "srli.w") == 0 || strcmp(mn, "srli.d") == 0 ||
        strcmp(mn, "srai.w") == 0 || strcmp(mn, "srai.d") == 0 ||
        strcmp(mn, "rotri.w") == 0 || strcmp(mn, "rotri.d") == 0) {
        uint8_t ui5 = (inst >> 10) & 0x1f;
        return snprintf(buf, bufsize, "%-8s r%d, r%d, %u", mn, rd, rj, ui5);
    }

    if (strncmp(mn, "alsl.", 5) == 0) {
        uint8_t sa2 = (inst >> 15) & 0x3;
        return snprintf(buf, bufsize, "%-8s r%d, r%d, r%d, %u", mn, rd, rj, rk, sa2 + 1);
    }

    if (strncmp(mn, "bstrins.", 8) == 0 || strncmp(mn, "bstrpick.", 9) == 0) {
        uint8_t msb = (inst >> 16) & 0x1f;
        uint8_t lsb = (inst >> 10) & 0x1f;
        return snprintf(buf, bufsize, "%-8s r%d, r%d, %u, %u", mn, rd, rj, msb, lsb);
    }

    if (strncmp(mn, "crc.", 4) == 0 || strncmp(mn, "crcc.", 5) == 0)
        return snprintf(buf, bufsize, "%-8s r%d, r%d, r%d", mn, rd, rj, rk);

    if (strcmp(mn, "csrrd") == 0 || strcmp(mn, "csrwr") == 0 || strcmp(mn, "csrxchg") == 0) {
        uint16_t csr = (inst >> 10) & 0x3ff;
        return snprintf(buf, bufsize, "%-8s r%d, 0x%x, r%d", mn, rd, csr, rj);
    }

    return snprintf(buf, bufsize, "%-8s r%d, r%d, r%d", mn, rd, rj, rk);
}

#define IR2_LINE_MAX 80

static void ir2_side_by_side(FILE *f,
                             const uint8_t *rec_ir2, uint32_t rec_len,
                             const uint8_t *aot_ir2, uint32_t aot_len) {
    uint32_t rec_cnt = rec_len / 4;
    uint32_t aot_cnt = aot_len / 4;
    uint32_t max_cnt = rec_cnt > aot_cnt ? rec_cnt : aot_cnt;

    fprintf(f, "      %-4s  %-40s | %-4s  %-40s\n",
            "REC#", "RECORD (JIT)", "AOT#", "AOT FILE");
    fprintf(f, "      %-4s  %-40s | %-4s  %-40s\n",
            "----", "----------------------------------------",
            "----", "----------------------------------------");

    for (uint32_t i = 0; i < max_cnt; i++) {
        char left[IR2_LINE_MAX] = "";
        char right[IR2_LINE_MAX] = "";

        if (i < rec_cnt) {
            uint32_t inst = read_u32_le(rec_ir2 + i * 4);
            ir2_disasm(left, IR2_LINE_MAX, inst);
        }

        if (i < aot_cnt) {
            uint32_t inst = read_u32_le(aot_ir2 + i * 4);
            ir2_disasm(right, IR2_LINE_MAX, inst);
        }

        int mismatch = 0;
        if (i < rec_cnt && i < aot_cnt) {
            mismatch = memcmp(rec_ir2 + i * 4, aot_ir2 + i * 4, 4) != 0;
        } else {
            mismatch = 1;
        }

        if (i < rec_cnt && i < aot_cnt) {
            fprintf(f, "  %s %-4u  %-40s | %-4u  %-40s\n",
                    mismatch ? "!=" : "  ", i, left, i, right);
        } else if (i < rec_cnt) {
            fprintf(f, "  != %-4u  %-40s | %-4s  %-40s\n",
                    i, left, "--", "(absent)");
        } else {
            fprintf(f, "  != %-4s  %-40s | %-4u  %-40s\n",
                    "--", "(absent)", i, right);
        }
    }
}

/* ============================================================
 * Report generation
 * ============================================================ */

static void generate_report(const char *report_path) {
    FILE *f;
    if (report_path) {
        f = fopen(report_path, "w");
        if (!f) {
            fprintf(stderr, "Error: cannot create report file '%s': %s\n",
                    report_path, strerror(errno));
            return;
        }
    } else {
        f = stdout;
    }

    fprintf(f, "============================================================\n");
    fprintf(f, "  AOT Verification Report\n");
    fprintf(f, "============================================================\n\n");

    /* Summary */
    int total_records = g_record_count;
    int total_aot = 0;
    for (int i = 0; i < g_entry_count; i++) {
        if (g_tb_entries[i].source == 1) total_aot++;
    }
    int total_mismatched = g_mismatch_count;
    int total_unmatched_rec = g_unmatched_rec_count;
    int total_unmatched_aot = g_unmatched_aot_count;
    int mismatch_ir1 = 0, mismatch_ir2 = 0;
    for (int i = 0; i < g_mismatch_count; i++) {
        if (g_mismatches[i].mismatch_type & 4) mismatch_ir1++;
        if (g_mismatches[i].mismatch_type & 2) mismatch_ir2++;
    }

    fprintf(f, "Summary:\n");
    fprintf(f, "  TB Records:        %d\n", total_records);
    fprintf(f, "  AOT TBs:           %d\n", total_aot);
    fprintf(f, "  Matched:           %d\n", g_total_matched);
    fprintf(f, "  Mismatched:        %d  (IR1=%d  IR2=%d)\n",
            total_mismatched, mismatch_ir1, mismatch_ir2);
    fprintf(f, "  Unmatched (rec):   %d  (in record but not in AOT)\n", total_unmatched_rec);
    fprintf(f, "  Unmatched (aot):   %d  (in AOT but not in record)\n", total_unmatched_aot);

    int verified_ok = g_total_matched - total_mismatched;
    fprintf(f, "  Verified OK:       %d\n", verified_ok);

    if (total_mismatched == 0 && total_unmatched_rec == 0 && total_unmatched_aot == 0) {
        fprintf(f, "\n  Result: PASS — All TBs verified correctly.\n");
    } else {
        fprintf(f, "\n  Result: FAIL — Issues detected.\n");
    }

    /* Mismatch details */
    if (total_mismatched > 0) {
        fprintf(f, "\n------------------------------------------------------------\n");
        fprintf(f, "  Mismatch Details (%d)\n", total_mismatched);
        fprintf(f, "------------------------------------------------------------\n");

        for (int i = 0; i < g_mismatch_count; i++) {
            mismatch_detail *md = &g_mismatches[i];
            fprintf(f, "\n  TB PC=0x%016" PRIx64 " (cs_base=0x%08x flags=0x%08x cflags=0x%08x)\n",
                    md->pc, md->cs_base, md->flags, md->cflags);

            if (md->mismatch_type & 1) {
                fprintf(f, "    [METADATA MISMATCH]\n");
                fprintf(f, "      IR2 num:  record=%u  aot=%u\n",
                        md->rec_ir2_num, md->aot_ir2_num);
                fprintf(f, "      IR2 len:  record=%u bytes  aot=%u bytes\n",
                        md->rec_ir2_len, md->aot_ir2_len);
            }

            if (md->mismatch_type & 2) {
                fprintf(f, "    [IR2 CODE MISMATCH]\n");
                ir2_side_by_side(f, md->rec_ir2, md->rec_ir2_len,
                                     md->aot_ir2, md->aot_ir2_len);
            }

            if (md->mismatch_type & 4) {
                fprintf(f, "    [IR1 CODE MISMATCH] (%u bytes)\n", md->rec_ir1_len);
                fprintf(f, "      record:");
                for (uint32_t k = 0; k < md->rec_ir1_len; k++)
                    fprintf(f, " %02x", md->rec_ir1[k]);
                fprintf(f, "\n");
                fprintf(f, "      aot:   ");
                for (uint32_t k = 0; k < md->rec_ir1_len; k++)
                    fprintf(f, " %02x", md->aot_ir1[k]);
                fprintf(f, "\n");
                fprintf(f, "      diff:  ");
                for (uint32_t k = 0; k < md->rec_ir1_len; k++)
                    fprintf(f, " %s", md->rec_ir1[k] == md->aot_ir1[k] ? "  " : "^^");
                fprintf(f, "\n");
            }
        }
    }

    /* Unmatched records */
    if (total_unmatched_rec > 0) {
        fprintf(f, "\n------------------------------------------------------------\n");
        fprintf(f, "  Unmatched: In Record but NOT in AOT (%d)\n", total_unmatched_rec);
        fprintf(f, "------------------------------------------------------------\n");
        for (int i = 0; i < g_unmatched_rec_count; i++) {
            tb_entry *u = &g_unmatched_rec[i];
            fprintf(f, "  PC=0x%016" PRIx64 " cs_base=0x%08x flags=0x%08x cflags=0x%08x\n",
                    u->pc, u->cs_base, u->flags, u->cflags);
        }
    }

    /* Unmatched AOT */
    if (total_unmatched_aot > 0) {
        fprintf(f, "\n------------------------------------------------------------\n");
        fprintf(f, "  Unmatched: In AOT but NOT in Record (%d)\n", total_unmatched_aot);
        fprintf(f, "------------------------------------------------------------\n");
        for (int i = 0; i < g_unmatched_aot_count; i++) {
            tb_entry *u = &g_unmatched_aot[i];
            fprintf(f, "  PC=0x%016" PRIx64 " cs_base=0x%08x flags=0x%08x cflags=0x%08x\n",
                    u->pc, u->cs_base, u->flags, u->cflags);
        }
    }

    fprintf(f, "\n============================================================\n");
    fprintf(f, "  End of Report\n");
    fprintf(f, "============================================================\n");

    if (report_path) {
        fclose(f);
        printf("Report written to: %s\n", report_path);
    }
}

/* ============================================================
 * Cleanup
 * ============================================================ */

static void cleanup(void) {
    for (int i = 0; i < g_record_count; i++) {
        free(g_records[i].ir1_data);
        free(g_records[i].ir2_data);
    }
}

/* ============================================================
 * Main / CLI
 * ============================================================ */

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <tb-record-file> <aot-core-file> [aot-user-file] [--report output.txt]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "  tb-record-file   Binary TB record file produced by tb-record module\n");
    fprintf(stderr, "  aot-core-file    AOT file for kernel-mode (CPL=0) TBs\n");
    fprintf(stderr, "  aot-user-file    AOT file for user-mode (CPL=3) TBs (optional)\n");
    fprintf(stderr, "  --report FILE    Write verification report to FILE (default: stdout)\n");
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char *tb_record_path = NULL;
    const char *aot_core_path  = NULL;
    const char *aot_user_path  = NULL;
    const char *report_path    = NULL;

    /* Parse arguments */
    int pos_args = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--report") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --report requires a filename argument\n");
                return 1;
            }
            report_path = argv[++i];
        } else {
            if (pos_args == 0) tb_record_path = argv[i];
            else if (pos_args == 1) aot_core_path = argv[i];
            else if (pos_args == 2) aot_user_path = argv[i];
            else {
                fprintf(stderr, "Error: too many positional arguments\n");
                print_usage(argv[0]);
                return 1;
            }
            pos_args++;
        }
    }

    if (!tb_record_path || !aot_core_path) {
        fprintf(stderr, "Error: tb-record-file and aot-core-file are required\n");
        print_usage(argv[0]);
        return 1;
    }

    printf("AOT Verification Tool\n");
    printf("=====================\n\n");

    /* Step 1: Parse TB record file */
    if (parse_tb_record_file(tb_record_path) != 0) {
        return 1;
    }

    /* Step 2: Parse AOT core file */
    if (parse_aot_file(aot_core_path) != 0) {
        cleanup();
        return 1;
    }

    /* Step 3: Parse AOT user file (optional) */
    if (aot_user_path) {
        if (parse_aot_file(aot_user_path) != 0) {
            cleanup();
            return 1;
        }
    }

    /* Step 4: Compare */
    compare_entries();

    /* Step 5: Generate report */
    generate_report(report_path);

    /* Cleanup */
    cleanup();

    /* Return non-zero if verification failed */
    if (g_mismatch_count > 0 || g_unmatched_rec_count > 0 || g_unmatched_aot_count > 0) {
        return 2;
    }
    return 0;
}
