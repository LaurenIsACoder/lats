/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TU_PAGE_SIZE 65536
#define LEAF_OFFSET 0
#define SCALAR_WRITE_OFFSET 32
#define SSE_WRITE_OFFSET 48
#define SSE_SOURCE_OFFSET 80

static void put_u32(unsigned char *p, uint32_t value)
{
    memcpy(p, &value, sizeof(value));
}

static void make_entry_stub(unsigned char *entry, size_t offset,
                            size_t target_offset)
{
    uint32_t displacement =
        (uint32_t)(TU_PAGE_SIZE + target_offset - (offset + 8));

    entry[offset] = 0x85;             /* test edi, edi */
    entry[offset + 1] = 0xff;
    entry[offset + 2] = 0x0f;         /* je member page */
    entry[offset + 3] = 0x84;
    put_u32(entry + offset + 4, displacement);
    entry[offset + 8] = 0xc3;         /* ret */
}

static int run_case(int use_sse)
{
    const size_t size = TU_PAGE_SIZE * 2;
    unsigned char *entry = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (entry == MAP_FAILED) {
        return 2;
    }

    unsigned char *member = entry + TU_PAGE_SIZE;
    int (*leaf)(int) = (void *)(entry + LEAF_OFFSET);
    int (*writer)(int) = (void *)(entry + 16);

    member[LEAF_OFFSET] = 0xb8;       /* mov eax, 1 */
    put_u32(member + LEAF_OFFSET + 1, 1);
    member[LEAF_OFFSET + 5] = 0xc3;   /* ret */
    memset(member + LEAF_OFFSET + 6, 0x90, 10);

    make_entry_stub(entry, 0, LEAF_OFFSET);
    make_entry_stub(entry, 16, use_sse ? SSE_WRITE_OFFSET : SCALAR_WRITE_OFFSET);

    if (use_sse) {
        member[SSE_WRITE_OFFSET] = 0xf3;     /* movdqu xmm0, [rip+source] */
        member[SSE_WRITE_OFFSET + 1] = 0x0f;
        member[SSE_WRITE_OFFSET + 2] = 0x6f;
        member[SSE_WRITE_OFFSET + 3] = 0x05;
        put_u32(member + SSE_WRITE_OFFSET + 4,
                SSE_SOURCE_OFFSET - (SSE_WRITE_OFFSET + 8));

        member[SSE_WRITE_OFFSET + 8] = 0xf3; /* movdqu [rip+leaf], xmm0 */
        member[SSE_WRITE_OFFSET + 9] = 0x0f;
        member[SSE_WRITE_OFFSET + 10] = 0x7f;
        member[SSE_WRITE_OFFSET + 11] = 0x05;
        put_u32(member + SSE_WRITE_OFFSET + 12,
                (uint32_t)(LEAF_OFFSET - (SSE_WRITE_OFFSET + 16)));
        member[SSE_WRITE_OFFSET + 16] = 0xc3;

        member[SSE_SOURCE_OFFSET] = 0xb8;
        put_u32(member + SSE_SOURCE_OFFSET + 1, 2);
        member[SSE_SOURCE_OFFSET + 5] = 0xc3;
        memset(member + SSE_SOURCE_OFFSET + 6, 0x90, 10);
    } else {
        member[SCALAR_WRITE_OFFSET] = 0xc6;  /* mov byte [rip+leaf+1], 2 */
        member[SCALAR_WRITE_OFFSET + 1] = 0x05;
        put_u32(member + SCALAR_WRITE_OFFSET + 2,
                (uint32_t)(LEAF_OFFSET + 1 -
                           (SCALAR_WRITE_OFFSET + 7)));
        member[SCALAR_WRITE_OFFSET + 6] = 2;
        member[SCALAR_WRITE_OFFSET + 7] = 0xc3;
    }

    int before = leaf(0);
    writer(0);
    int after = leaf(0);
    int failed = before != 1 || after != 2;

    printf("%s before=%d after=%d failed=%d\n",
           use_sse ? "sse" : "scalar", before, after, failed);
    if (munmap(entry, size)) {
        return 3;
    }
    return failed;
}

int main(void)
{
    int failed = run_case(0) + run_case(1);

    printf("TU_RWX_MEMBER_WRITE FAILED=%d\n", failed);
    return failed != 0;
}
