/* SPDX-License-Identifier: GPL-2.0-only */
#include "qemu/osdep.h"
#include "target/i386/latx/ir1/ir1-decode.c"

typedef union DecodeBuffer {
    uint64_t align;
    uint8_t bytes[2 * IR1_INST_SIZE];
} DecodeBuffer;

static csh decoders[3];
static unsigned int checked;
static unsigned int failures;

static void mismatch(const uint8_t *code, const char *field,
                     uint64_t actual, uint64_t expected)
{
    static unsigned int shown[256];
    unsigned int opcode =
        code[0] >= 0x40 && code[0] <= 0x4f ? code[1] : code[0];
    failures++;
    if (shown[opcode]++ < 4) {
        fprintf(stderr, "%s: got 0x%" PRIx64 " expected 0x%" PRIx64 " bytes:",
                field, actual, expected);
        for (int i = 0; i < 15; i++) {
            fprintf(stderr, " %02x", code[i]);
        }
        fprintf(stderr, "\n");
    }
}

#define SAME(field) do { \
    if (fast->field != full->field) { \
        mismatch(code, #field, fast->field, full->field); \
    } \
} while (0)

static void compare(const uint8_t *code, uint64_t address)
{
    DecodeBuffer a = { 0 }, b = { 0 };
    cs_insn *fast = ir1_hot_decode(code, address, 1, a.bytes, 64);
    cs_insn *full = NULL;
    if (!fast) {
        return;
    }
    checked++;
    if (cs_disasm(decoders[2], code, 15, address, 1, &full, 1, b.bytes) != 1) {
        mismatch(code, "valid", 1, 0);
        return;
    }
    SAME(id);
    SAME(size);
    SAME(address);
    SAME(detail->x86.rex);
    SAME(detail->x86.addr_size);
    SAME(detail->x86.modrm);
    SAME(detail->x86.sib);
    SAME(detail->x86.disp);
    SAME(detail->x86.sib_index);
    SAME(detail->x86.sib_scale);
    SAME(detail->x86.sib_base);
    SAME(detail->x86.encoding.modrm_offset);
    SAME(detail->x86.encoding.disp_offset);
    SAME(detail->x86.encoding.disp_size);
    SAME(detail->x86.encoding.imm_offset);
    SAME(detail->x86.encoding.imm_size);
    SAME(detail->x86.op_count);
    SAME(detail->x86.eflags);
    for (size_t i = 0; i < sizeof(cs_x86); i++) {
        const uint8_t *x = (void *)&fast->detail->x86;
        const uint8_t *y = (void *)&full->detail->x86;
        if (x[i] != y[i]) {
            mismatch(code, "x86 detail byte", x[i], y[i]);
            break;
        }
    }
    g_assert_cmpmem(fast->bytes, fast->size, full->bytes, full->size);
}

static void test_cache(void)
{
    const int modes[] = { 16, 32, 64 };
    uint8_t code[15] = { 0x48, 0x0f, 0xaf, 0xc1 };
    DecodeBuffer expected[3] = { 0 }, output = { 0 };
    cs_insn *full[3], *actual;
    for (int m = 0; m < 3; m++) {
        g_assert_cmpuint(cs_disasm(decoders[m], code, 15, 0x1000, 1,
            &full[m], 0, expected[m].bytes), ==, 1);
        ir1_decode_template_insert(code, 15, modes[m], full[m]);
    }
    for (int m = 0; m < 3; m++) {
        actual = ir1_decode_template_lookup(code, 15, 0x2000, 1,
                                            output.bytes, modes[m]);
        g_assert_nonnull(actual);
        g_assert_true(actual->detail == (void *)(actual + 1));
        g_assert_cmpuint(actual->id, ==, full[m]->id);
        g_assert_cmpuint(actual->size, ==, full[m]->size);
        g_assert_cmpuint(actual->address, ==, 0x2000);
        g_assert_cmpmem(actual->detail, sizeof(cs_detail),
                        full[m]->detail, sizeof(cs_detail));
        /* Translation-local edits must not mutate the cached raw detail. */
        actual->detail->x86.operands[0].size = 99;
        actual = ir1_decode_template_lookup(code, 15, 0x3000, 1,
                                            output.bytes, modes[m]);
        g_assert_cmpmem(actual->detail, sizeof(cs_detail),
                        full[m]->detail, sizeof(cs_detail));
    }
    uint8_t a[15] = { 0x0f, 0x1f, 0x84, 0, 0x11, 0x22, 0x33, 0x44 };
    uint8_t b[15];
    memcpy(b, a, sizeof(b));
    b[7] ^= 1;
    g_assert_cmpuint(ir1_decode_template_hash(a, 4, 64), ==,
                     ir1_decode_template_hash(b, 4, 64));
    for (int i = 0; i < 2; i++) {
        uint8_t *bytes = i ? b : a;
        g_assert_cmpuint(cs_disasm(decoders[2], bytes, 15, 0, 1,
            &actual, 0, output.bytes), ==, 1);
        ir1_decode_template_insert(bytes, 15, 64, actual);
    }
    g_assert_null(ir1_decode_template_lookup(a, 15, 0, 0, output.bytes, 64));
    g_assert_nonnull(ir1_decode_template_lookup(b, 15, 0, 0, output.bytes, 64));
    b[8] ^= 1;
    g_assert_nonnull(ir1_decode_template_lookup(b, 15, 0, 0, output.bytes, 64));
    g_assert_null(ir1_decode_template_lookup(b, 7, 0, 0, output.bytes, 64));
    actual = (void *)output.bytes;
    actual->detail->x86.operands[0].type = X86_OP_IMM;
    actual->detail->x86.op_count = 1;
    g_assert_false(ir1_decode_template_is_address_independent(actual));
    actual->detail->x86.operands[0].type = X86_OP_MEM;
    actual->detail->x86.operands[0].mem.base = X86_REG_RIP;
    g_assert_false(ir1_decode_template_is_address_independent(actual));
}

static gpointer cache_worker(gpointer opaque)
{
    const cs_insn *expected = opaque;
    DecodeBuffer output = { 0 };
    g_assert_null(ir1_decode_templates);
    g_assert_null(ir1_decode_template_valid);
    ir1_decode_template_insert(expected->bytes, 15, 64, expected);
    cs_insn *actual = ir1_decode_template_lookup(expected->bytes, 15,
        expected->address, 0, output.bytes, 64);
    g_assert_nonnull(actual);
    g_assert_cmpmem(actual->detail, sizeof(cs_detail),
                    expected->detail, sizeof(cs_detail));
    return NULL;
}

static void test_threads(void)
{
    uint8_t code[15] = { 0x0f, 0xaf, 0xc1 };
    DecodeBuffer buffer = { 0 };
    cs_insn *expected;
    g_assert_cmpuint(cs_disasm(decoders[2], code, 15, 0x1000, 1,
        &expected, 0, buffer.bytes), ==, 1);
    for (int i = 0; i < 32; i++) {
        GThread *thread = g_thread_new("decode-cache", cache_worker, expected);
        g_thread_join(thread);
    }
}

static void test_frontend(void)
{
    const int modes[] = { 16, 32, 64 };
    /* Include unsupported prefixes, relative operands and invalid encodings. */
    const uint8_t cases[][15] = {
        { 0x48, 0x89, 0xd8 }, { 0x0f, 0xaf, 0xc1 },
        { 0xe8, 0x80, 0xff, 0xff, 0xff },
        { 0x64, 0x48, 0x8b, 0x05, 0x80, 0xff, 0xff, 0xff },
        { 0x66, 0x0f, 0xaf, 0xc1 }, { 0x0f, 0xff },
    };
    for (int m = 0; m < 3; m++) {
        for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
            for (int repeat = 0; repeat < 3; repeat++) {
                uint64_t address = 0x1000 + repeat * 0x100;
                DecodeBuffer a = { 0 }, b = { 0 };
                cs_insn *actual = NULL, *expected = NULL;
                size_t count = cs_disasm(decoders[m], cases[i], 15,
                    address, 1, &expected, 0, a.bytes);
                size_t got = latxs_decode_one(decoders[m], cases[i], address,
                    modes[m], &actual, 0, b.bytes);
                g_assert_cmpuint(got, ==, count);
                if (!count) {
                    g_assert_null(actual);
                    continue;
                }
                g_assert_cmpuint(actual->id, ==, expected->id);
                g_assert_cmpuint(actual->size, ==, expected->size);
                g_assert_cmpuint(actual->address, ==, expected->address);
                g_assert_cmpmem(actual->detail, sizeof(cs_detail),
                                expected->detail, sizeof(cs_detail));
            }
        }
    }
}

int main(void)
{
    const cs_mode modes[] = { CS_MODE_16, CS_MODE_32, CS_MODE_64 };
    for (int i = 0; i < 3; i++) {
        g_assert_cmpint(cs_open(CS_ARCH_X86, modes[i], &decoders[i]), ==, 0);
        cs_option(decoders[i], CS_OPT_DETAIL, CS_OPT_ON);
    }
    uint8_t code[15];
    for (int r = -1; r < 16; r++) {
        for (int op = 0; op < 256; op++) {
            for (int modrm = 0; modrm < 256; modrm++) {
                memset(code, modrm & 1 ? 0x81 : 0x7e, sizeof(code));
                int n = 0;
                if (r >= 0) {
                    code[n++] = 0x40 + r;
                }
                code[n++] = op;
                code[n] = modrm;
                compare(code, UINT64_C(0x7fff12345678));
            }
        }
    }
    for (int r = 0; r < 16; r++) {
        for (int mod = 0; mod < 3; mod++) {
            for (int sib = 0; sib < 256; sib++) {
                memset(code, 0x80, sizeof(code));
                code[0] = 0x40 + r;
                code[1] = 0x8b;
                code[2] = (mod << 6) | 4;
                code[3] = sib;
                compare(code, UINT64_MAX - 4);
            }
        }
    }
    uint32_t random = 0x12345678;
    for (int sample = 0; sample < 500000; sample++) {
        for (int i = 0; i < 15; i++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            code[i] = random;
        }
        compare(code, sample & 1 ? 0 : UINT64_MAX - 4);
    }
    test_cache();
    test_threads();
    test_frontend();
    g_assert_cmpuint(checked, >, 250000);
    printf("fast decoder checked=%u field mismatches=%u\n", checked, failures);
    return failures ? 1 : 0;
}
