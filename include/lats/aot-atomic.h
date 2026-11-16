/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LATS_AOT_ATOMIC_H
#define LATS_AOT_ATOMIC_H

#include <stdbool.h>
#include <stdint.h>

enum LatsAotAtomicOperation {
    LATS_ATOMIC_ADD = 1, LATS_ATOMIC_OR, LATS_ATOMIC_ADC, LATS_ATOMIC_SBB,
    LATS_ATOMIC_AND, LATS_ATOMIC_SUB, LATS_ATOMIC_XOR, LATS_ATOMIC_XCHG,
    LATS_ATOMIC_CMPXCHG, LATS_ATOMIC_XADD, LATS_ATOMIC_INC, LATS_ATOMIC_DEC,
    LATS_ATOMIC_NOT, LATS_ATOMIC_NEG, LATS_ATOMIC_BIT,
};

#define LATS_ATOMIC_OPERATION(d) ((d) & 15U)
#define LATS_ATOMIC_BYTES(d) (1U << (((d) >> 4) & 3U))
#define LATS_ATOMIC_REGISTER(d) (((d) >> 6) & 15U)
/* BIT uses the result-register field for BTS/BTR/BTC (0/1/2). */
#define LATS_ATOMIC_BIT_KIND(d) LATS_ATOMIC_REGISTER(d)
#define LATS_ATOMIC_HIGH_BYTE(d) (((d) >> 10) & 1U)
#define LATS_ATOMIC_PAIR(d) (LATS_ATOMIC_OPERATION(d) == LATS_ATOMIC_CMPXCHG && \
                             LATS_ATOMIC_BYTES(d) == 8 && LATS_ATOMIC_HIGH_BYTE(d))
#define LATS_ATOMIC_LENGTH(d) (((d) >> 11) & 15U)
/* Returning helpers preserve the active LLVM caller stack. */
#define LATS_ATOMIC_RETURN (1U << 15)

/* A native HAMT atomic resumes at the second branch after a failed page walk.
 * Both result registers are early-clobber outputs; only accessed is defined
 * on failure. The fixed six-word sequence never expands a guest access. */
#define LATS_HAMT_ATOMIC_WORDS 6
static inline bool lats_aot_hamt_atomic_sequence(const uint32_t *code)
{
    uint32_t opcode = code[0] & UINT32_C(0xffff8000);
    unsigned result = code[3] & 31;
    bool atomic =
        (opcode >= UINT32_C(0x385a0000) && opcode <= UINT32_C(0x385b8000)) ||
        (opcode >= UINT32_C(0x385e0000) && opcode <= UINT32_C(0x385f8000)) ||
        (opcode >= UINT32_C(0x38690000) && opcode <= UINT32_C(0x386d8000));
    return atomic && result && result != (code[0] & 31) &&
        result != ((code[0] >> 5) & 31) && result != ((code[0] >> 10) & 31) &&
        code[1] == UINT32_C(0x50000800) && code[2] == UINT32_C(0x50000c00) &&
        code[3] == (UINT32_C(0x03800400) | result) &&
        code[4] == UINT32_C(0x50000800) &&
        code[5] == (UINT32_C(0x03800000) | result);
}

/* The descriptor identifies an instruction's operation and register result.
 * Its address and source value are supplied by the compiled function. This
 * decoder is used while compiling/loading saved code, never to execute a TB. */
static inline uint32_t lats_aot_atomic_decode(const uint8_t *code, unsigned size)
{
    unsigned at = 0, rex = 0, operand16 = 0;
    bool locked = false;
    if (size < 2 || size > 15) return 0;
    while (at < size) {
        unsigned byte = code[at];
        if (byte >= 0x40 && byte <= 0x4f) {
            rex = byte;
        } else {
            if (byte == 0xf0) locked = true;
            else if (byte == 0x66) operand16 = 1;
            else if (byte != 0x26 && byte != 0x2e && byte != 0x36 &&
                     byte != 0x3e && byte != 0x64 && byte != 0x65 && byte != 0x67)
                break;
            rex = 0;
        }
        at++;
    }
    if (at >= size) return 0;
    unsigned opcode = code[at++], operation = 0, immediate = 0, bit_kind = 0;
    bool byte_operand = !(opcode & 1), exchange_result = false, pair = false;
    static const uint8_t arithmetic[8] = {
        LATS_ATOMIC_ADD, LATS_ATOMIC_OR, LATS_ATOMIC_ADC, LATS_ATOMIC_SBB,
        LATS_ATOMIC_AND, LATS_ATOMIC_SUB, LATS_ATOMIC_XOR, 0,
    };
    if (opcode < 0x40 && (opcode & 7) <= 1) {
        operation = arithmetic[opcode >> 3];
    } else if (opcode == 0x86 || opcode == 0x87) {
        operation = LATS_ATOMIC_XCHG;
        exchange_result = true;
    } else if (opcode == 0x0f) {
        if (at >= size) return 0;
        opcode = code[at++];
        byte_operand = !(opcode & 1);
        if (opcode == 0xb0 || opcode == 0xb1) operation = LATS_ATOMIC_CMPXCHG;
        else if (opcode == 0xc0 || opcode == 0xc1) {
            operation = LATS_ATOMIC_XADD;
            exchange_result = true;
        } else if (opcode == 0xc7 && !(rex & 8)) {
            operation = LATS_ATOMIC_CMPXCHG;
            byte_operand = false;
            pair = true;
        } else if (opcode == 0xab || opcode == 0xb3 || opcode == 0xbb || opcode == 0xba) {
            operation = LATS_ATOMIC_BIT;
            byte_operand = false;
            bit_kind = opcode == 0xab ? 0 : opcode == 0xb3 ? 1 : 2;
        } else return 0;
    } else if (opcode != 0x80 && opcode != 0x81 && opcode != 0x83 &&
               opcode != 0xfe && opcode != 0xff && opcode != 0xf6 && opcode != 0xf7)
        return 0;
    if (at >= size || (!locked && operation != LATS_ATOMIC_XCHG)) return 0;
    unsigned modrm = code[at++], mod = modrm >> 6, rm = modrm & 7;
    unsigned group = (modrm >> 3) & 7;
    if (mod == 3 || (pair && group != 1)) return 0;
    if (opcode == 0x80 || opcode == 0x81 || opcode == 0x83) {
        operation = arithmetic[group];
        immediate = opcode == 0x81 ? ((rex & 8) || !operand16 ? 4 : 2) : 1;
        byte_operand = opcode == 0x80;
    } else if (opcode == 0xfe || opcode == 0xff) {
        if (group > 1) return 0;
        operation = group ? LATS_ATOMIC_DEC : LATS_ATOMIC_INC;
    } else if (opcode == 0xf6 || opcode == 0xf7) {
        if (group != 2 && group != 3) return 0;
        operation = group == 2 ? LATS_ATOMIC_NOT : LATS_ATOMIC_NEG;
    }
    if (operation == LATS_ATOMIC_BIT && opcode == 0xba) {
        if (group < 5) return 0;
        bit_kind = group - 5;
        immediate = 1;
    }
    if (!operation) return 0;
    unsigned width = pair ? 3 : byte_operand ? 0 : (rex & 8) ? 3 : operand16 ? 1 : 2;
    unsigned result_reg = operation == LATS_ATOMIC_BIT ? bit_kind : 0, high_byte = pair;
    if (exchange_result) {
        result_reg = group | ((rex & 4) ? 8 : 0);
        if (!width && !rex && result_reg >= 4) {
            result_reg -= 4;
            high_byte = 1;
        }
    }
    if (rm == 4) {
        if (at >= size) return 0;
        unsigned sib = code[at++];
        if (!mod && (sib & 7) == 5) at += 4;
    } else if (!mod && rm == 5) at += 4;
    if (mod == 1) at++;
    else if (mod == 2) at += 4;
    if (at + immediate != size) return 0;
    return operation | (width << 4) | (result_reg << 6) | (high_byte << 10) | (size << 11);
}

#endif
