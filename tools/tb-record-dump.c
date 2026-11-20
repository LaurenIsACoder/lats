/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#define TB_RECORD_MAGIC   0x54425243U
#define TB_RECORD_VERSION 1U

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

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-n COUNT] [-h] <record-file>\n"
        "  -n COUNT   Dump first COUNT records (0 = all, default: 10)\n"
        "  -h         Show this help\n",
        prog);
}

static void hex_dump(const uint8_t *data, int len, int indent)
{
    for (int i = 0; i < len; i++) {
        if (i % 16 == 0) {
            if (i > 0) printf("\n");
            printf("%*s", indent, "");
        }
        printf("%02x ", data[i]);
    }
    if (len > 0) printf("\n");
}

static int dump_record(FILE *fp, int index)
{
    TBRecordHeader hdr;
    long pos = ftell(fp);

    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        if (feof(fp)) return 0;
        fprintf(stderr, "Error: failed to read record %d header at offset 0x%lx\n",
                index, pos);
        return -1;
    }

    printf("  [%d] pc=0x%016lx cs_base=0x%08x flags=0x%08x cflags=0x%08x\n",
           index, (unsigned long)hdr.pc, hdr.cs_base, hdr.flags, hdr.cflags);
    printf("      size=%u icount=%u page0=0x%016lx page1=0x%016lx\n",
           hdr.size, hdr.icount,
           (unsigned long)hdr.page_addr0, (unsigned long)hdr.page_addr1);
    printf("      ir1_len=%u ir2_len=%u\n", hdr.ir1_len, hdr.ir2_len);

    if (hdr.ir1_len > 0) {
        uint8_t *ir1 = malloc(hdr.ir1_len);
        if (!ir1) {
            fprintf(stderr, "Error: malloc failed for ir1 (%u bytes)\n", hdr.ir1_len);
            return -1;
        }
        if (fread(ir1, 1, hdr.ir1_len, fp) != hdr.ir1_len) {
            fprintf(stderr, "Error: failed to read ir1_data (%u bytes)\n", hdr.ir1_len);
            free(ir1);
            return -1;
        }
        printf("      ir1 (x86 bytes):\n");
        hex_dump(ir1, hdr.ir1_len, 8);
        free(ir1);
    }

    if (hdr.ir2_len > 0) {
        uint8_t *ir2 = malloc(hdr.ir2_len);
        if (!ir2) {
            fprintf(stderr, "Error: malloc failed for ir2 (%u bytes)\n", hdr.ir2_len);
            return -1;
        }
        if (fread(ir2, 1, hdr.ir2_len, fp) != hdr.ir2_len) {
            fprintf(stderr, "Error: failed to read ir2_data (%u bytes)\n", hdr.ir2_len);
            free(ir2);
            return -1;
        }
        printf("      ir2 (LoongArch instructions, %u bytes):\n", hdr.ir2_len);
        hex_dump(ir2, hdr.ir2_len, 8);
        free(ir2);
    }

    return 1;
}

int main(int argc, char **argv)
{
    int dump_count = 10;
    int opt;

    while ((opt = getopt(argc, argv, "n:h")) != -1) {
        switch (opt) {
        case 'n':
            dump_count = atoi(optarg);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: no record file specified\n");
        usage(argv[0]);
        return 1;
    }

    const char *path = argv[optind];
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", path, strerror(errno));
        return 1;
    }

    uint32_t header[4];
    if (fread(header, sizeof(header), 1, fp) != 1) {
        fprintf(stderr, "Error: failed to read file header\n");
        fclose(fp);
        return 1;
    }

    if (header[0] != TB_RECORD_MAGIC) {
        fprintf(stderr, "Error: bad magic 0x%08x (expected 0x%08x)\n",
                header[0], TB_RECORD_MAGIC);
        fclose(fp);
        return 1;
    }

    printf("File: %s\n", path);
    printf("  magic:   0x%08x\n", header[0]);
    printf("  version: %u\n", header[1]);
    printf("  records: %u\n", header[2]);
    printf("  reserved: %u\n\n", header[3]);

    int total = (int)header[2];
    int to_dump = (dump_count == 0) ? total : dump_count;
    if (to_dump > total) to_dump = total;

    printf("Dumping %d of %d records:\n\n", to_dump, total);

    int dumped = 0;
    for (int i = 0; i < to_dump; i++) {
        int ret = dump_record(fp, i);
        if (ret < 0) break;
        if (ret == 0) {
            fprintf(stderr, "Warning: unexpected EOF at record %d\n", i);
            break;
        }
        printf("\n");
        dumped++;
    }

    printf("Dumped %d records.\n", dumped);
    fclose(fp);
    return 0;
}
