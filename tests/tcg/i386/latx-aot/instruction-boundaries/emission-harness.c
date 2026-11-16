/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Test support for extracted production emitters, not a standalone program. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int IR2_OPND;
typedef struct { int offset; } IR1_OPND;
typedef struct { int unused; } IR1_INST;
enum {
    LISA_ADD_D, LISA_ADD_W, LISA_ADDI_D, LISA_ADDI_W,
    LISA_ST_W, LISA_SLLI_D, lisa_mov32z,
};
enum { esp_index = 1, cs_index, X86_REG_SS, X86_REG_ESP, EXMode_Z };
#define lsassert assert

static uint64_t regs[64];
static bool defined[64];
static int next_reg, stack_size, loads;
static IR1_OPND sp_ir1_opnd;
static IR2_OPND latxs_env_ir2_opnd;
static struct {
    struct { bool code64, ss32; } sys;
    int ignore_eip_update;
} td;
static struct { typeof(td) *tr_data; } env = { &td }, *lsenv = &env;

static uint64_t read_reg(int reg)
{
    assert(defined[reg] && "emitter reads an undefined temporary");
    return regs[reg];
}

static void write_reg(int reg, uint64_t value)
{
    regs[reg] = value;
    defined[reg] = true;
}

static int64_t sextract64(uint64_t value, int start, int bits)
{
    return (int64_t)(value << (64 - start - bits)) >> (64 - bits);
}

static IR2_OPND latxs_ra_alloc_gpr(int reg) { return reg; }
static IR2_OPND latxs_ra_alloc_itemp(void)
{
    assert(next_reg < 64);
    return next_reg++;
}
static void latxs_ra_free_temp(IR2_OPND *reg) { }

static void latxs_load_imm64_to_ir2(IR2_OPND *rd, uint64_t value)
{
    write_reg(*rd, value);
}

static void latxs_append_ir2_opnd3(int op, IR2_OPND *rd,
                                 IR2_OPND *rj, IR2_OPND *rk)
{
    assert(op == LISA_ADD_D || op == LISA_ADD_W);
    uint64_t value = read_reg(*rj) + read_reg(*rk);
    if (op == LISA_ADD_W) {
        value = (int64_t)(int32_t)value;
    }
    write_reg(*rd, value);
}

static void latxs_append_ir2_opnd2i(int op, IR2_OPND *rd,
                                  IR2_OPND *rj, int64_t immediate)
{
    if (op == LISA_ST_W) {
        (void)read_reg(*rd);
        return;
    }
    if (op == LISA_SLLI_D) {
        assert(immediate >= 0 && immediate < 64);
        write_reg(*rd, read_reg(*rj) << immediate);
        return;
    }
    assert(op == LISA_ADDI_D || op == LISA_ADDI_W);
    assert(immediate >= -2048 && immediate <= 2047);
    uint64_t value = read_reg(*rj) + immediate;
    if (op == LISA_ADDI_W) {
        value = (int64_t)(int32_t)value;
    }
    write_reg(*rd, value);
}

static void latxs_append_ir2_opnd2_(int op, IR2_OPND *rd, IR2_OPND *rj)
{
    assert(op == lisa_mov32z);
    write_reg(*rd, (uint32_t)read_reg(*rj));
}

static void latxs_store_ir2_to_ir1_gpr(IR2_OPND *value, IR1_OPND *dest)
{
    assert(dest == &sp_ir1_opnd);
    write_reg(esp_index, (read_reg(esp_index) & ~UINT64_C(0xffff)) |
                        (read_reg(*value) & 0xffff));
}

static int latxs_get_sys_stack_addr_size(void) { return stack_size; }
static void ir1_opnd_build_full_mem(IR1_OPND *mem, int width, int seg,
                                  int base, int offset, int index, int scale)
{
    mem->offset = offset;
}
static void latxs_load_ir1_mem_to_ir2(IR2_OPND *rd, IR1_OPND *mem,
                                    int mode, int size)
{
    assert(size == stack_size);
    assert(mem->offset == 0 || mem->offset == 2 || mem->offset == 4);
    write_reg(*rd, ++loads == 1 ? 0x1234 : 0x100);
}
static void latxs_tr_gen_store_to_curr_eip(IR2_OPND *value)
{
    assert(read_reg(*value) == 0x1234);
}
static int lsenv_offset_of_seg_selector(void *env, int seg) { return 0; }
static int lsenv_offset_of_seg_base(void *env, int seg) { return 4; }

/* PRODUCTION_FUNCTIONS */

static void reset(uint64_t sp, int size)
{
    memset(defined, 0, sizeof(defined));
    memset(&td, 0, sizeof(td));
    next_reg = 2;
    loads = 0;
    stack_size = size;
    td.sys.code64 = size == 8;
    td.sys.ss32 = size == 4;
    write_reg(esp_index, sp);
}

static uint64_t expected_sp(uint64_t initial, int64_t delta, int size)
{
    uint64_t result = initial + delta;
    if (size == 2) {
        return (initial & ~UINT64_C(0xffff)) | (result & 0xffff);
    }
    return size == 4 ? (uint32_t)result : result;
}

static void test_returns(bool far_return)
{
    const uint64_t seeds[] = {0, 0x1000, 0xffff, 0x12345678fffffff0};
    unsigned checks = 0;
    for (int size = 2; size <= (far_return ? 4 : 8); size *= 2) {
        for (int width = 16; width <= (size == 8 ? 64 : 32); width *= 2) {
            for (unsigned seed = 0; seed < sizeof(seeds) / sizeof(seeds[0]); seed++) {
                for (int imm = 0; imm <= 65535; imm++) {
                    int delta = imm + (width / 8) * (far_return ? 2 : 1);
                    reset(seeds[seed], size);
                    if (far_return) {
                        latxs_do_translate_ret_far_real(NULL, width, imm);
                        assert(loads == 2 && td.ignore_eip_update);
                    } else {
                        latxs_update_stack_pointer(delta, size);
                    }
                    assert(regs[esp_index] == expected_sp(seeds[seed], delta, size));
                    checks++;
                }
            }
        }
    }
    /* Other callers use negative stack deltas. */
    if (!far_return) {
        const int deltas[] = {-65536, -4096, -2049, -2048, -2047, -8, 0};
        for (int size = 2; size <= 8; size *= 2) {
            for (unsigned i = 0; i < sizeof(deltas) / sizeof(deltas[0]); i++) {
                reset(seeds[3], size);
                latxs_update_stack_pointer(deltas[i], size);
                assert(regs[esp_index] == expected_sp(seeds[3], deltas[i], size));
                checks++;
            }
        }
    }
    printf("PASS %s return emitter: %u checks\n", far_return ? "far" : "near", checks);
}

static void check_mask(int32_t value)
{
    bool low;
    uint32_t pos;
    if (imm_mask(value, &low, &pos)) {
        assert(pos < 32);
        uint64_t mask = (UINT64_C(1) << pos) - 1;
        assert((low ? mask : ~mask) == (uint64_t)(int64_t)value);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "near")) {
        test_returns(false);
    } else if (!strcmp(argv[1], "far")) {
        test_returns(true);
    } else {
        assert(!strcmp(argv[1], "mask"));
        bool low;
        uint32_t pos;
        assert(!imm_mask(0, &low, &pos));
        for (int value = -32768; value <= 32767; value++) {
            check_mask(value);
        }
        for (int bit = 0; bit < 32; bit++) {
            uint32_t mask = (UINT32_C(1) << bit) - 1;
            check_mask(mask);
            check_mask(~mask);
        }
        uint32_t random = 0x12345678;
        for (int i = 0; i < 10000; i++) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            check_mask(random);
        }
        puts("PASS AND mask semantics and UBSan: 75601 checks");
    }
}
