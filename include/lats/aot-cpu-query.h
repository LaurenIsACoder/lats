/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LATS_AOT_CPU_QUERY_H
#define LATS_AOT_CPU_QUERY_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

enum {
    LATS_CPU_QUERY_CPUID = 1,
    LATS_CPU_QUERY_RDTSC,
    LATS_CPU_QUERY_XGETBV,
    LATS_CPU_QUERY_PAUSE,
};

#define LATS_CPU_QUERY_KIND(value) ((value) & 255)
#define LATS_CPU_QUERY_LENGTH(value) ((value) >> 8)

/* Return the complete encoded length and operation, or zero for other forms. */
static inline unsigned lats_aot_cpu_query_decode(const uint8_t *bytes, size_t size)
{
    size_t at = 0;
    unsigned kind;
    bool operand_size = false;

    if (size < 2 || size > 15) {
        return 0;
    }
    while (at < size) {
        unsigned byte = bytes[at];
        if (byte == 0x66) {
            operand_size = true;
        } else if (byte != 0x26 && byte != 0x2e && byte != 0x36 &&
                   byte != 0x3e && byte != 0x64 && byte != 0x65 &&
                   byte != 0x67 && (byte < 0x40 || byte > 0x4f)) {
            break;
        }
        at++;
    }
    if (size - at == 2 && bytes[at] == 0x0f && bytes[at + 1] == 0xa2) {
        kind = LATS_CPU_QUERY_CPUID;
    } else if (size - at == 2 && bytes[at] == 0x0f && bytes[at + 1] == 0x31) {
        kind = LATS_CPU_QUERY_RDTSC;
    } else if (size - at == 3 && bytes[at] == 0x0f && bytes[at + 1] == 0x01 &&
               bytes[at + 2] == 0xd0 && !operand_size) {
        kind = LATS_CPU_QUERY_XGETBV;
    } else if (size - at == 2 && bytes[at] == 0xf3 && bytes[at + 1] == 0x90) {
        kind = LATS_CPU_QUERY_PAUSE;
    } else {
        return 0;
    }
    return (unsigned)(size << 8) | kind;
}

#endif
