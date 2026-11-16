/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LATS_AOT_SYSTEM_H
#define LATS_AOT_SYSTEM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum LatsAotSystemOperation {
    LATS_SYSTEM_PCMPISTRI = 1, LATS_SYSTEM_PCMPISTRM,
    LATS_SYSTEM_PCMPESTRI, LATS_SYSTEM_PCMPESTRM,
    LATS_SYSTEM_LAR, LATS_SYSTEM_LSL, LATS_SYSTEM_VERR, LATS_SYSTEM_VERW,
    LATS_SYSTEM_CMPXCHG8B, LATS_SYSTEM_CMPXCHG16B,
};
#define LATS_SYSTEM_KIND(d) ((d) & 15U)
#define LATS_SYSTEM_LENGTH(d) (((d) >> 4) & 15U)
#define LATS_SYSTEM_DEST(d) (((d) >> 8) & 15U)
#define LATS_SYSTEM_SOURCE(d) (((d) >> 12) & 15U)
#define LATS_SYSTEM_MEMORY(d) (((d) >> 16) & 1U)
#define LATS_SYSTEM_WIDTH(d) (((d) >> 17) & 3U)
#define LATS_SYSTEM_CONTROL(d) (((d) >> 19) & 511U)

/* Saved encodings identify only operations already using a C helper in LATS.
 * LLVM supplies the effective memory address or the loaded 16-bit selector. */
static inline unsigned lats_aot_system_decode(const uint8_t *code, size_t size)
{
    unsigned at = 0, rex = 0, operand16 = 0, opcode, kind = 0, control = 0;
    bool locked = false, sse = false, immediate = false;
    if (size < 3 || size > 15) return 0;
    while (at < size) {
        unsigned byte = code[at];
        if (byte >= 0x40 && byte <= 0x4f) rex = byte;
        else {
            if (byte == 0x66) operand16 = 1;
            else if (byte == 0xf0) locked = true;
            else if (byte != 0x26 && byte != 0x2e && byte != 0x36 && byte != 0x3e &&
                     byte != 0x64 && byte != 0x65 && byte != 0x67) break;
            rex = 0;
        }
        at++;
    }
    if (size - at < 3 || code[at++] != 0x0f || locked) return 0;
    opcode = code[at++];
    if (opcode == 0x3a) {
        if (at >= size || !operand16) return 0;
        opcode = code[at++];
        switch (opcode) {
        case 0x60: kind = LATS_SYSTEM_PCMPESTRM; break;
        case 0x61: kind = LATS_SYSTEM_PCMPESTRI; break;
        case 0x62: kind = LATS_SYSTEM_PCMPISTRM; break;
        case 0x63: kind = LATS_SYSTEM_PCMPISTRI; break;
        default: return 0;
        }
        sse = immediate = true;
    } else if (opcode == 2) kind = LATS_SYSTEM_LAR;
    else if (opcode == 3) kind = LATS_SYSTEM_LSL;
    else if (opcode != 0 && opcode != 0xc7) return 0;
    if (at >= size) return 0;
    unsigned modrm = code[at++], mod = modrm >> 6, group = (modrm >> 3) & 7, rm = modrm & 7;
    if (opcode == 0) {
        if (group != 4 && group != 5) return 0;
        kind = group == 4 ? LATS_SYSTEM_VERR : LATS_SYSTEM_VERW;
    } else if (opcode == 0xc7) {
        if (group != 1 || mod == 3) return 0;
        kind = rex & 8 ? LATS_SYSTEM_CMPXCHG16B : LATS_SYSTEM_CMPXCHG8B;
    }
    if (mod != 3) {
        if (rm == 4) {
            if (at >= size) return 0;
            unsigned sib = code[at++];
            if (!mod && (sib & 7) == 5) at += 4;
        } else if (!mod && rm == 5) at += 4;
        at += mod == 1 ? 1 : mod == 2 ? 4 : 0;
    }
    if (immediate) {
        if (at >= size) return 0;
        control = code[at++] | ((rex & 8) ? 256 : 0);
    }
    if (!kind || at != size) return 0;
    unsigned destination = group | ((rex & 4) ? 8 : 0);
    unsigned source = rm | ((rex & 1) ? 8 : 0);
    unsigned width = rex & 8 ? 2 : operand16 ? 0 : 1;
    return kind | (size << 4) | (destination << 8) | (source << 12) |
           ((mod != 3) << 16) | ((sse ? 0 : width) << 17) | (control << 19);
}

static inline bool lats_aot_system_valid(uint64_t descriptor)
{
    unsigned kind = LATS_SYSTEM_KIND(descriptor);
    return !(descriptor >> 28) && kind >= LATS_SYSTEM_PCMPISTRI &&
           kind <= LATS_SYSTEM_CMPXCHG16B && LATS_SYSTEM_LENGTH(descriptor) >= 3 &&
           LATS_SYSTEM_LENGTH(descriptor) <= 15 && LATS_SYSTEM_WIDTH(descriptor) <= 2;
}
#endif
