/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LATS_AOT_X87_H
#define LATS_AOT_X87_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

enum {
    LATS_X87_FLD_EXT = 1,
    LATS_X87_FSTP_EXT,
    LATS_X87_FLD_REG,
    LATS_X87_FSTP_REG,
    LATS_X87_FABS,
    LATS_X87_FXCH,
    LATS_X87_FXAM,
    LATS_X87_FUCOMI,
    LATS_X87_FUCOMIP,
    LATS_X87_FCOMI,
    LATS_X87_FCOMIP,
    LATS_X87_FNSTSW_AX,
    LATS_X87_FNSTCW_MEM,
    LATS_X87_FPREM, LATS_X87_FPREM1, LATS_X87_FXTRACT,
    LATS_X87_FSIN, LATS_X87_FCOS, LATS_X87_FPATAN, LATS_X87_FSINCOS,
    LATS_X87_FPTAN, LATS_X87_FYL2X, LATS_X87_FYL2XP1, LATS_X87_F2XM1,
    LATS_X87_FBLD, LATS_X87_FBSTP, LATS_X87_FIST_STORE,
    LATS_X87_FISTTP_STORE, LATS_X87_FILD_I64, LATS_X87_FWAIT,
    LATS_X87_FNOP,
};

#define LATS_X87_KIND(value) ((value) & 31)
#define LATS_X87_REGISTER(value) (((value) >> 5) & 7)
#define LATS_X87_LENGTH(value) ((value) >> 8)

static inline bool lats_aot_x87_valid(uint64_t descriptor)
{
    unsigned kind = LATS_X87_KIND(descriptor);
    unsigned reg = LATS_X87_REGISTER(descriptor);
    uint64_t length = LATS_X87_LENGTH(descriptor);
    return length >= 1 && length <= 15 && kind >= LATS_X87_FLD_EXT &&
           kind <= LATS_X87_FNOP &&
           (length != 1 || kind == LATS_X87_FWAIT) &&
           (kind != LATS_X87_FIST_STORE || (reg & 3) <= 2) &&
           (kind != LATS_X87_FISTTP_STORE || reg <= 2) &&
           (!reg || kind == LATS_X87_FIST_STORE || kind == LATS_X87_FISTTP_STORE || kind == LATS_X87_FLD_REG || kind == LATS_X87_FSTP_REG ||
            kind == LATS_X87_FXCH || (kind >= LATS_X87_FUCOMI && kind <= LATS_X87_FCOMIP));
}

/* Decode the supported helper operations without a runtime disassembler.
 * Memory addresses are computed by LLVM from the same saved instruction. */
static inline unsigned lats_aot_x87_decode(const uint8_t *bytes, size_t size)
{
    size_t at = 0;
    unsigned kind = 0, reg = 0;

    if (size < 1 || size > 15) {
        return 0;
    }
    while (at < size) {
        unsigned byte = bytes[at];
        if (byte != 0x26 && byte != 0x2e && byte != 0x36 && byte != 0x3e &&
            byte != 0x64 && byte != 0x65 && byte != 0x66 && byte != 0x67 &&
            (byte < 0x40 || byte > 0x4f)) {
            break;
        }
        at++;
    }
    if (size - at == 1 && bytes[at] == 0x9b) {
        return (unsigned)(size << 8) | LATS_X87_FWAIT;
    }
    if (size - at < 2) {
        return 0;
    }
    unsigned opcode = bytes[at++], modrm = bytes[at++];
    unsigned mod = modrm >> 6, group = (modrm >> 3) & 7, rm = modrm & 7;
    if (mod == 3) {
        reg = rm;
        if (opcode == 0xd9 && group == 0) {
            kind = LATS_X87_FLD_REG;
        } else if (opcode == 0xdd && group == 3) {
            kind = LATS_X87_FSTP_REG;
        } else if (opcode == 0xd9 && group == 1) {
            kind = LATS_X87_FXCH;
        } else if (opcode == 0xd9 && modrm == 0xe1) {
            kind = LATS_X87_FABS;
            reg = 0;
        } else if (opcode == 0xd9 && modrm == 0xe5) {
            kind = LATS_X87_FXAM;
            reg = 0;
        } else if (opcode == 0xdf && modrm == 0xe0) {
            kind = LATS_X87_FNSTSW_AX;
            reg = 0;
        } else if (opcode == 0xd9 && modrm == 0xd0) {
            kind = LATS_X87_FNOP;
            reg = 0;
        } else if (opcode == 0xd9 && modrm >= 0xf0) {
            static const uint8_t operations[16] = {
                LATS_X87_F2XM1, LATS_X87_FYL2X, LATS_X87_FPTAN, LATS_X87_FPATAN,
                LATS_X87_FXTRACT, LATS_X87_FPREM1, 0, 0,
                LATS_X87_FPREM, LATS_X87_FYL2XP1, 0, LATS_X87_FSINCOS,
                0, 0, LATS_X87_FSIN, LATS_X87_FCOS
            };
            kind = operations[modrm - 0xf0];
            reg = 0;
        } else if (opcode == 0xdb && group == 5) {
            kind = LATS_X87_FUCOMI;
        } else if (opcode == 0xdf && group == 5) {
            kind = LATS_X87_FUCOMIP;
        } else if (opcode == 0xdb && group == 6) {
            kind = LATS_X87_FCOMI;
        } else if (opcode == 0xdf && group == 6) {
            kind = LATS_X87_FCOMIP;
        }
    } else {
        if (opcode == 0xdb && group == 5) {
            kind = LATS_X87_FLD_EXT;
        } else if (opcode == 0xdb && group == 7) {
            kind = LATS_X87_FSTP_EXT;
        } else if (opcode == 0xd9 && group == 7) {
            kind = LATS_X87_FNSTCW_MEM;
        }
        if (opcode == 0xdf && group == 4) kind = LATS_X87_FBLD;
        else if (opcode == 0xdf && group == 6) kind = LATS_X87_FBSTP;
        else if ((opcode == 0xdf || opcode == 0xdb) && (group == 2 || group == 3)) {
            kind = LATS_X87_FIST_STORE;
            reg = (opcode == 0xdf ? 0 : 1) | (group == 3 ? 4 : 0);
        } else if (opcode == 0xdf && group == 7) {
            kind = LATS_X87_FIST_STORE;
            reg = 2 | 4;
        } else if ((opcode == 0xdf || opcode == 0xdb || opcode == 0xdd) && group == 1) {
            kind = LATS_X87_FISTTP_STORE;
            reg = opcode == 0xdf ? 0 : opcode == 0xdb ? 1 : 2;
        } else if (opcode == 0xdf && group == 5) kind = LATS_X87_FILD_I64;
        if (rm == 4) {
            if (at == size) {
                return 0;
            }
            unsigned base = bytes[at++] & 7;
            if (!mod && base == 5) {
                at += 4;
            }
        } else if (!mod && rm == 5) {
            at += 4;
        }
        at += mod == 1 ? 1 : mod == 2 ? 4 : 0;
    }
    return kind && at == size ? (unsigned)(size << 8) | (reg << 5) | kind : 0;
}

#endif
