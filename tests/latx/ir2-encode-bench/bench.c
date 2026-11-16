/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Standalone encoder correctness/throughput test; no translator launcher. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "encoder-types.h"

typedef uint32_t (*encoder)(IR2_INST *);
static encoder encoders[] = {encode_base, encode_reduce, encode_table, encode_both};
static const char *names[] = {"base", "reduce", "table", "both"};
static uint64_t rng = UINT64_C(0x831ad7c42f086519);

static uint32_t random_word(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)rng;
}

static void die(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(1);
}

/* Independently place each operand's bits using the original format table.
 * No merged table, vector shuffle, signed shift or vector reduction is used. */
static uint32_t reference(const IR2_INST *in)
{
    if (in->_opcode == LISA_DUP) {
        return (uint32_t)in->_opnd[0].val;
    }
    const GM_LA_OPCODE_FORMAT *fmt = &lisa_format_table[in->_opcode - LISA_INVALID];
    uint32_t word = fmt->opcode;
    for (unsigned j = 0; j < 4; j++) {
        const GM_OPERAND_PLACE_RELATION *field = &bit_field_table_new[fmt->opnd[j]];
        uint32_t value = (uint32_t)in->_opnd[j].val;
        for (int bit = 0; bit < field->bit_range_0.len; bit++) {
            word |= ((value >> bit) & 1U) << (field->bit_range_0.start + bit);
        }
        for (int bit = 0; bit < field->bit_range_1.len; bit++) {
            word |= ((value >> (field->bit_range_0.len + bit)) & 1U) <<
                    (field->bit_range_1.start + bit);
        }
    }
    return word;
}

static int supported(unsigned opcode)
{
    return opcode == LISA_DUP || lisa_format_table[opcode - LISA_INVALID].opcode;
}

static uint64_t checked;
static void check(IR2_INST *in)
{
    uint32_t want = reference(in);
    for (unsigned v = 0; v < 4; v++) {
        uint32_t got = encoders[v](in);
        if (got != want) {
            fprintf(stderr, "FAIL variant=%s opcode=%d input=%08x,%08x,%08x,%08x "
                    "expected=%08x actual=%08x\n", names[v], in->_opcode,
                    (uint32_t)in->_opnd[0].val, (uint32_t)in->_opnd[1].val,
                    (uint32_t)in->_opnd[2].val, (uint32_t)in->_opnd[3].val, want, got);
            exit(1);
        }
    }
    checked++;
}

static void verify(void)
{
    static const uint32_t boundaries[] = {
        0, 1, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256,
        0x7ff, 0x800, 0xfff, 0x1000, 0x7fff, 0x8000, 0xffff, 0x10000,
        0xfffff, 0x100000, 0x1fffff, 0x200000, 0x3ffffff, 0x4000000,
        0x7fffffff, 0x80000000, 0xfffffffe, 0xffffffff
    };
    unsigned opcodes = 0;
    for (unsigned op = LISA_INVALID; op < LISA_ENDING; op++) {
        if (!supported(op)) {
            continue;
        }
        opcodes++;
        IR2_INST in = {._opcode = op};
        for (unsigned j = 0; j < 4; j++) {
            for (unsigned b = 0; b < sizeof(boundaries) / sizeof(boundaries[0]); b++) {
                memset(in._opnd, 0, sizeof(in._opnd));
                in._opnd[j].val = (int32_t)boundaries[b];
                check(&in);
            }
            for (unsigned bit = 0; bit < 32; bit++) {
                memset(in._opnd, 0, sizeof(in._opnd));
                in._opnd[j].val = (int32_t)(UINT32_C(1) << bit);
                check(&in);
            }
        }
        for (unsigned r = 0; r < 128; r++) {
            for (unsigned j = 0; j < 4; j++) {
                in._opnd[j].val = (int32_t)random_word();
                in._opnd[j]._type = IR2_OPND_IMMD;
            }
            check(&in);
        }
    }
    IR2_INST golden = {._opcode = LISA_ADD_D};
    golden._opnd[0].val = 5;
    golden._opnd[1].val = 6;
    golden._opnd[2].val = 7;
    if (reference(&golden) != (0x00108000U | 5U | (6U << 5) | (7U << 10))) {
        die("FAIL known ADD.D encoding");
    }
    printf("VERIFY opcodes=%u vectors=%" PRIu64 " encoder_comparisons=%" PRIu64
           " ir2_size=%zu operand_offset=%zu PASS\n", opcodes, checked,
           checked * 4, sizeof(IR2_INST), offsetof(IR2_INST, _opnd));
}

enum { CORPUS_SIZE = 4096 };
static IR2_INST corpus[CORPUS_SIZE];
static uint32_t output[CORPUS_SIZE];

static void make_corpus(const char *kind)
{
    /* Synthetic mixes; these are not a recorded Java instruction trace. */
    static const unsigned hot[] = {
        LISA_ADD_D, LISA_ADDI_D, LISA_SUB_D, LISA_LD_D, LISA_ST_D,
        LISA_LD_W, LISA_LD_WU, LISA_ST_W, LISA_AND, LISA_ANDI,
        LISA_OR, LISA_ORI, LISA_XOR, LISA_XORI, LISA_SLLI_D,
        LISA_SRLI_D, LISA_SRAI_D, LISA_B, LISA_BL, LISA_BEQ,
        LISA_BNE, LISA_BLT, LISA_BGEU, LISA_JIRL, LISA_LU12I_W,
        LISA_LU32I_D, LISA_LU52I_D, LISA_SLT, LISA_SLTU, LISA_DUP
    };
    unsigned available[LISA_ENDING - LISA_INVALID], count = 0;
    if (!strcmp(kind, "hot")) {
        memcpy(available, hot, sizeof(hot));
        count = sizeof(hot) / sizeof(hot[0]);
    } else if (!strcmp(kind, "wide")) {
        for (unsigned op = LISA_INVALID; op < LISA_ENDING; op++) {
            if (supported(op)) {
                available[count++] = op;
            }
        }
    } else {
        die("corpus must be hot or wide");
    }
    for (unsigned i = 0; i < CORPUS_SIZE; i++) {
        IR2_INST *in = &corpus[i];
        in->_opcode = available[random_word() % count];
        const GM_LA_OPCODE_FORMAT *fmt = &lisa_format_table[in->_opcode - LISA_INVALID];
        for (unsigned j = 0; j < 4; j++) {
            const GM_OPERAND_PLACE_RELATION *field = &bit_field_table_new[fmt->opnd[j]];
            unsigned bits = field->bit_range_0.len + field->bit_range_1.len;
            uint32_t mask = bits ? (UINT32_C(1) << bits) - 1 : 0;
            in->_opnd[j]._type = IR2_OPND_IMMD;
            in->_opnd[j].val = in->_opcode == LISA_DUP ? (int32_t)random_word() :
                              (int32_t)(random_word() & mask);
        }
    }
}

/* Separate translation units prevent folding the encoder or its inputs.
 * Write code words as the real emitter does, without a checksum dependency
 * in the timed loop. Each mode has the same direct-call loop. */
#define LOOP(name) \
static void run_##name(unsigned passes) \
{ \
    for (unsigned pass = 0; pass < passes; pass++) { \
        for (unsigned i = 0; i < CORPUS_SIZE; i++) { \
            output[i] = encode_##name(&corpus[i]); \
        } \
    } \
}
LOOP(base)
LOOP(reduce)
LOOP(table)
LOOP(both)

static uint64_t now_ns(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &time)) {
        die("clock_gettime failed");
    }
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "verify")) {
        verify();
        return 0;
    }
    if (argc != 4) {
        die("usage: bench verify OR bench base|reduce|table|both hot|wide passes");
    }
    unsigned variant;
    for (variant = 0; variant < 4 && strcmp(argv[1], names[variant]); variant++) { }
    if (variant == 4) {
        die("unknown variant");
    }
    char *end;
    errno = 0;
    unsigned long passes = strtoul(argv[3], &end, 10);
    if (errno || *end || passes == 0 || passes > 10000000) {
        die("passes must be 1..10000000");
    }
    make_corpus(argv[2]);
    void (*runs[])(unsigned) = {run_base, run_reduce, run_table, run_both};
    runs[variant](16);
    uint64_t start = now_ns();
    runs[variant](passes);
    uint64_t elapsed = now_ns() - start;
    uint64_t checksum = 0;
    for (unsigned i = 0; i < CORPUS_SIZE; i++) {
        if (output[i] != reference(&corpus[i])) {
            die("FAIL timed output differs from reference");
        }
        checksum = checksum * 33 + output[i];
    }
    uint64_t operations = (uint64_t)passes * CORPUS_SIZE;
    printf("RESULT variant=%s corpus=%s operations=%" PRIu64 " elapsed_ns=%" PRIu64
           " ns_per_op=%.6f million_ops_per_s=%.6f checksum=%016" PRIx64 " PASS\n",
           names[variant], argv[2], operations, elapsed, (double)elapsed / operations,
           (double)operations * 1000 / elapsed, checksum);
    return 0;
}
