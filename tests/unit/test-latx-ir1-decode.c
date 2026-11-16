/* SPDX-License-Identifier: GPL-2.0-only */
#include "qemu/osdep.h"
#include "target/i386/latx/include/latx-options.h"
#include "target/i386/latx/ir1/ir1-decode.c"

int option_ir1_cache_ways = LATX_IR1_CACHE_WAYS_DEFAULT;
int option_ir1_cache_replace = LATX_IR1_CACHE_REPLACE_LRU;
int option_ir1_cache_enabled = 1;
int option_ir1_cache_mode = LATX_IR1_CACHE_MODE_ON;
uint64_t option_ir1_cache_entries;
int option_ir1_timing;
int option_hot_decode_enabled = 1;
int option_ir1_distribution;
int option_ir1_profile;
int option_ir1_fast_cache_entries = LATX_IR1_FAST_CACHE_ENTRIES_DEFAULT;
int option_ir1_hash = LATX_IR1_HASH_MURMUR;

typedef union DecodeBuffer {
    uint64_t align;
    uint8_t bytes[2 * IR1_INST_SIZE];
} DecodeBuffer;

static void clear_fast_cache(void)
{
    if (ir1_decode_fast_templates) {
        Ir1DecodeFastEntry *templates = ir1_decode_fast_templates;

        ir1_decode_fast_templates = NULL;
        g_private_set(&ir1_decode_fast_templates_owner, NULL);
        qemu_vfree(templates);
    }
#if IR1_DECODE_FAST_CACHE_WAYS > 1
    if (ir1_decode_fast_policy) {
        uint8_t *policy = ir1_decode_fast_policy;

        ir1_decode_fast_policy = NULL;
        g_private_set(&ir1_decode_fast_policy_owner, NULL);
        g_free(policy);
    }
#endif
}

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

static void compare_mode(const uint8_t *code, uint64_t address, int mode)
{
    DecodeBuffer a = { 0 }, b = { 0 };
    csh decoder = mode == 32 ? decoders[1] : decoders[2];
    cs_insn *fast = ir1_hot_decode(code, address, 1, a.bytes, mode);
    cs_insn *full = NULL;
    if (!fast) {
        return;
    }
    checked++;
    if (cs_disasm(decoder, code, 15, address, 1, &full, 1, b.bytes) != 1) {
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
    for (size_t i = 0; i < sizeof(cs_detail); i++) {
        const uint8_t *x = (void *)fast->detail;
        const uint8_t *y = (void *)full->detail;
        if (x[i] != y[i]) {
            mismatch(code, "detail byte", x[i], y[i]);
            break;
        }
    }
    if (fast->size != full->size ||
        memcmp(fast->bytes, full->bytes, full->size) != 0) {
        mismatch(code, "bytes", fast->size, full->size);
    }
}

static void require_hot_hit(const uint8_t *code, uint64_t address, int mode)
{
    DecodeBuffer output = { 0 };

    g_assert_nonnull(ir1_hot_decode(code, address, 1, output.bytes, mode));
    compare_mode(code, address, mode);
}

static void test_hot_decode_32(void)
{
    /* Exercise every instruction family accepted by the hot decoder. */
    const uint8_t cases[][15] = {
        { 0x70, 0x10 },                         /* JO rel8 */
        { 0x7f, 0xf0 },                         /* JG rel8 */
        { 0x0f, 0x80, 0x10, 0x00, 0x00, 0x00 }, /* JO rel32 */
        { 0x0f, 0x8f, 0xf0, 0xff, 0xff, 0xff }, /* JG rel32 */
        { 0xe8, 0x10, 0x00, 0x00, 0x00 },       /* CALL rel32 */
        { 0xe9, 0xf0, 0xff, 0xff, 0xff },       /* JMP rel32 */
        { 0xeb, 0x10 },                         /* JMP rel8 */
        { 0x90 },                               /* NOP */
        { 0xc3 },                               /* RET */
        { 0x50 },                               /* PUSH EAX */
        { 0x5f },                               /* POP EDI */
        { 0xb8, 0x78, 0x56, 0x34, 0x12 },       /* MOV EAX,imm32 */
        { 0xbf, 0xef, 0xbe, 0xad, 0xde },       /* MOV EDI,imm32 */
        { 0x83, 0xc0, 0x7f },                   /* ADD EAX,imm8 */
        { 0x83, 0xe8, 0x80 },                   /* SUB EAX,imm8 */
        { 0x83, 0xf8, 0x55 },                   /* CMP EAX,imm8 */
        { 0x89, 0xd8 },                         /* MOV EAX,EBX */
        { 0x8b, 0xcb },                         /* MOV ECX,EBX */
        { 0x8d, 0x44, 0x8b, 0x10 },             /* LEA EAX,[EBX+ECX*4+16] */
        { 0x85, 0xd8 },                         /* TEST EAX,EBX */
        { 0x31, 0xd8 },                         /* XOR EAX,EBX */
        { 0x33, 0xc1 },                         /* XOR EAX,ECX */
        { 0x39, 0xd8 },                         /* CMP EAX,EBX */
        { 0x3b, 0xc1 },                         /* CMP EAX,ECX */
        { 0x01, 0xd8 },                         /* ADD EAX,EBX */
        { 0x03, 0xc1 },                         /* ADD EAX,ECX */
        { 0x29, 0xd8 },                         /* SUB EAX,EBX */
        { 0x2b, 0xc1 },                         /* SUB EAX,ECX */
        { 0x21, 0xd8 },                         /* AND EAX,EBX */
        { 0x23, 0xc1 },                         /* AND EAX,ECX */
        { 0x09, 0xd8 },                         /* OR EAX,EBX */
        { 0x0b, 0xc1 },                         /* OR EAX,ECX */
        { 0x89, 0x45, 0xf0 },                   /* MOV [EBP-16],EAX */
        { 0x8b, 0x84, 0x8b, 0x78, 0x56, 0x34,
          0x12 },                               /* MOV EAX,[EBX+ECX*4+disp32] */
        { 0x8b, 0x05, 0x78, 0x56, 0x34, 0x12 }, /* MOV EAX,[disp32] */
        { 0x8b, 0x04, 0x85, 0x78, 0x56, 0x34,
          0x12 },                               /* MOV EAX,[ECX*4+disp32] */
    };

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        require_hot_hit(cases[i], UINT64_C(0x76543210) + i * 0x40, 32);
    }
}

static void test_hot_rejections_32(void)
{
    const uint8_t rejected[][15] = {
        { 0x40 },                         /* INC EAX, not a REX prefix */
        { 0x4f },                         /* DEC EDI */
        { 0x66, 0x90 },                   /* operand-size prefix */
        { 0x67, 0x8b, 0x00 },             /* address-size prefix */
        { 0xf0, 0x90 },                   /* LOCK prefix */
        { 0xf2, 0x90 },                   /* REPNE prefix */
        { 0xf3, 0x90 },                   /* REP prefix */
        { 0x64, 0x8b, 0x00 },             /* segment prefix */
        { 0x8d, 0xc0 },                   /* LEA with register source */
        { 0x85, 0x00 },                   /* memory TEST left to Capstone */
    };
    DecodeBuffer output = { 0 };

    for (size_t i = 0; i < G_N_ELEMENTS(rejected); i++) {
        g_assert_null(ir1_hot_decode(rejected[i], 0x1000, 1,
                                     output.bytes, 32));
    }
    g_assert_null(ir1_hot_decode(rejected[0], 0x1000, 1,
                                 output.bytes, 16));
    g_assert_null(ir1_hot_decode(rejected[0], 0x1000, 1, NULL, 32));
}

static void test_hash_strategies(void)
{
    uint8_t code[15] = { 0x48, 0x8b, 0x44, 0x8b, 0x10 };
    uint8_t ff_a[15] = { 0xff, 0x00 };
    uint8_t ff_b[15] = { 0xff, 0xd0 };
    uint32_t legacy, murmur;
    uint64_t changed_bits = 0;
    uint32_t random = 0x12345678;

    option_ir1_hash = LATX_IR1_HASH_LEGACY;
    g_assert_cmpstr(latxs_ir1_hash_name(option_ir1_hash), ==, "legacy");
    legacy = ir1_decode_template_hash_value(code, sizeof(code), 64);

    option_ir1_hash = LATX_IR1_HASH_MURMUR;
    g_assert_cmpstr(latxs_ir1_hash_name(option_ir1_hash), ==, "murmur");
    murmur = ir1_decode_template_hash_value(code, sizeof(code), 64);
    g_assert_cmpuint(legacy, !=, murmur);
    g_assert_cmpuint(murmur, ==,
        ir1_decode_template_hash_value(code, sizeof(code), 64));

    option_ir1_hash = LATX_IR1_HASH_LEGACY;
    g_assert_cmpuint(ir1_decode_template_hash_value(ff_a, sizeof(ff_a), 32),
                     ==,
                     ir1_decode_template_hash_value(ff_b, sizeof(ff_b), 32));
    option_ir1_hash = LATX_IR1_HASH_MURMUR;
    g_assert_cmpuint(ir1_decode_template_hash_value(ff_a, sizeof(ff_a), 32),
                     !=,
                     ir1_decode_template_hash_value(ff_b, sizeof(ff_b), 32));

    for (int sample = 0; sample < 4096; sample++) {
        for (size_t i = 0; i < sizeof(code); i++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            code[i] = random;
        }
        code[0] ^= 1;
        uint32_t original = ir1_decode_template_hash_value(code, sizeof(code),
                                                            64);
        code[0] ^= 1;
        uint32_t flipped = ir1_decode_template_hash_value(code, sizeof(code),
                                                           64);
        changed_bits += __builtin_popcount((original ^ flipped) & 0x3fff);
    }
    g_assert_cmpuint(changed_bits / 4096, >=, 5);
    g_assert_cmpuint(changed_bits / 4096, <=, 9);
    option_ir1_hash = LATX_IR1_HASH_MURMUR;
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
        g_assert_true(actual->detail == IR1_INST_DETAIL(actual));
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
    uint8_t candidates[LATX_IR1_CACHE_MAX_WAYS + 1][15];
    memset(candidates, 0, sizeof(candidates));
    for (int i = 0; i < LATX_IR1_CACHE_MAX_WAYS + 1; i++) {
        candidates[i][0] = 0xc8;
        candidates[i][1] = i;
        candidates[i][2] = 0;
        candidates[i][3] = i;
    }
    for (int i = 1; i < LATX_IR1_CACHE_MAX_WAYS + 1; i++) {
        g_assert_cmpuint(ir1_decode_template_hash(candidates[0], 2, 64), ==,
                         ir1_decode_template_hash(candidates[i], 2, 64));
    }
    for (int i = 0; i < LATX_IR1_CACHE_MAX_WAYS + 1; i++) {
        uint8_t *bytes = candidates[i];
        g_assert_cmpuint(cs_disasm(decoders[2], bytes, 15, 0, 1,
            &actual, 0, output.bytes), ==, 1);
        ir1_decode_template_insert(bytes, 15, 64, actual);
    }
    int present = 0;
    for (int i = 0; i < LATX_IR1_CACHE_MAX_WAYS + 1; i++) {
        present += ir1_decode_template_lookup(candidates[i], 15, 0, 0,
                                              output.bytes, 64) != NULL;
    }
    g_assert_cmpint(present, ==, LATX_IR1_CACHE_MAX_WAYS);
    uint8_t *last = candidates[LATX_IR1_CACHE_MAX_WAYS];
    last[4] ^= 1;
    g_assert_nonnull(ir1_decode_template_lookup(last, 15, 0, 0,
                                                output.bytes, 64));
    g_assert_null(ir1_decode_template_lookup(last, 3, 0, 0,
                                             output.bytes, 64));
}

static gpointer distribution_worker(gpointer opaque)
{
    DecodeBuffer output = { 0 };
    uint8_t hit_code[15] = { 0x0f, 0xaf, 0xc1 };
    uint8_t miss_code[15] = { 0x0f, 0xaf, 0xd8 };
    cs_insn *expected = NULL, *actual = NULL;
    size_t set, miss_set;
    uint64_t total_lookups = 0, total_hits = 0, total_misses = 0;
    bool reuse_seen = false;
    Ir1DecodeDistribution *distribution;

    g_assert_null(ir1_decode_distribution);
    g_assert_cmpuint(cs_disasm(decoders[2], hit_code, 15, 0x12345000, 1,
                               &expected, 0, output.bytes), ==, 1);
    ir1_decode_template_insert(hit_code, 15, 64, expected);
    clear_fast_cache();

    actual = ir1_decode_template_lookup(hit_code, 15, 0x12345010, 1,
                                        output.bytes, 64);
    g_assert_nonnull(actual);
    g_assert_null(ir1_decode_template_lookup(miss_code, 15, 0x12345ff0, 1,
                                             output.bytes, 64));

    distribution = ir1_decode_distribution_get();
    g_assert_nonnull(distribution);
    g_assert_cmpuint(distribution->entries, ==, 64);
    g_assert_cmpuint(distribution->sets, ==, 16);
    g_assert_cmpuint(distribution->ways, ==, 4);

    set = ir1_decode_template_hash(hit_code, 15, 64);
    g_assert_cmpuint(distribution->set_profiles[set].hits, ==, 1);
    miss_set = ir1_decode_template_hash(miss_code, 15, 64);
    g_assert_cmpuint(distribution->set_profiles[miss_set].misses, ==, 1);
    for (size_t candidate = 0; candidate < distribution->sets; candidate++) {
        total_lookups += distribution->set_profiles[candidate].lookups;
        total_hits += distribution->set_profiles[candidate].hits;
        total_misses += distribution->set_profiles[candidate].misses;
    }
    g_assert_cmpuint(total_lookups, ==, 2);
    g_assert_cmpuint(total_hits, ==, 1);
    g_assert_cmpuint(total_misses, ==, 1);
    g_assert_cmpuint(distribution->set_occupancy[set], >=, 1);
    g_assert_cmpuint(distribution->opcode_hits[2][0x0f], ==, 1);
    g_assert_cmpuint(distribution->opcode_inserts[2][0x0f], ==, 1);
    g_assert_cmpuint(distribution->fast_way_hits + distribution->way_hits[0] +
                     distribution->way_hits[1] + distribution->way_hits[2] +
                     distribution->way_hits[3], ==, 1);

    for (size_t way = 0; way < distribution->ways; way++) {
        size_t index = set * distribution->ways + way;

        reuse_seen |= distribution->entry_reuse[index] == 1;
    }
    g_assert_true(reuse_seen);

    Ir1DecodePageProfile *page =
        ir1_decode_distribution_page(distribution, 0x12345010);
    g_assert_cmphex(page->page, ==, 0x12345000);
    g_assert_cmpuint(page->lookups, ==, 2);
    g_assert_cmpuint(page->hits, ==, 1);
    g_assert_cmpuint(page->misses, ==, 1);
    return NULL;
}

static void test_distribution(void)
{
    GThread *thread;

    option_ir1_cache_entries = 64;
    option_ir1_cache_ways = 4;
    option_ir1_distribution = 1;
    thread = g_thread_new("distribution", distribution_worker, NULL);
    g_thread_join(thread);
    option_ir1_distribution = 0;
    option_ir1_cache_entries = 0;
    option_ir1_cache_ways = LATX_IR1_CACHE_WAYS_DEFAULT;
}

static void test_cache_main_fallback(void)
{
    uint8_t code[15] = { 0x48, 0x0f, 0xaf, 0xc1 };
    DecodeBuffer expected_buf = { 0 }, output = { 0 };
    cs_insn *expected = NULL, *actual;

    clear_fast_cache();
    g_assert_cmpuint(cs_disasm(decoders[2], code, 15, 0x1000, 1,
        &expected, 0, expected_buf.bytes), ==, 1);
    ir1_decode_template_insert(code, 15, 64, expected);

    clear_fast_cache();
    actual = ir1_decode_template_lookup(code, 15, 0x2000, 1,
                                        output.bytes, 64);
    g_assert_nonnull(actual);
    g_assert_false(ir1_decode_last_fast_hit);
    g_assert_cmpuint(actual->address, ==, 0x2000);
    g_assert_cmpmem(actual->bytes, actual->size, code, expected->size);
    g_assert_cmpmem(actual->detail, sizeof(cs_detail),
                    expected->detail, sizeof(cs_detail));
}

static void test_cache_relocation(void)
{
    const int modes[] = { 32, 64 };
    uint8_t call[15] = { 0xe8, 0x10, 0x00, 0x00, 0x00 };
    uint8_t call2[15] = { 0xe8, 0x20, 0x00, 0x00, 0x00 };
    uint8_t mov[15] = { 0xb8, 0x78, 0x56, 0x34, 0x12 };
    uint8_t mov2[15] = { 0xb8, 0x21, 0x43, 0x65, 0x87 };
    uint8_t rel16[15] = { 0x66, 0xe9, 0x34, 0x12, 0x00, 0x00 };
    uint8_t rel16_next[15] = { 0x66, 0xe9, 0x14, 0x13, 0x00, 0x00 };
    uint8_t rip[15] = { 0x48, 0x8b, 0x05, 0x10, 0x00, 0x00, 0x00 };
    uint8_t rip2[15] = { 0x48, 0x8b, 0x05, 0x20, 0x00, 0x00, 0x00 };
    DecodeBuffer raw = { 0 }, expected_buf = { 0 }, output = { 0 };
    cs_insn *raw_insn = NULL, *expected = NULL, *actual = NULL;

    for (size_t i = 0; i < G_N_ELEMENTS(modes); i++) {
        int mode = modes[i];
        csh decoder = decoders[i + 1];

        g_assert_cmpuint(cs_disasm(decoder, call, 15, 0x1000, 1,
            &raw_insn, 0, raw.bytes), ==, 1);
        ir1_decode_template_insert(call, 15, mode, raw_insn);
        actual = ir1_decode_template_lookup(call2, 15, 0x2000, 1,
                                            output.bytes, mode);
        g_assert_nonnull(actual);
        g_assert_cmpuint(cs_disasm(decoder, call2, 15, 0x2000, 1,
            &expected, 0, expected_buf.bytes), ==, 1);
        g_assert_cmpmem(actual->detail, sizeof(cs_detail),
                        expected->detail, sizeof(cs_detail));

        g_assert_cmpuint(cs_disasm(decoder, mov, 15, 0x1000, 1,
            &raw_insn, 0, raw.bytes), ==, 1);
        ir1_decode_template_insert(mov, 15, mode, raw_insn);
        actual = ir1_decode_template_lookup(mov2, 15, 0x2000, 1,
                                            output.bytes, mode);
        g_assert_nonnull(actual);
        g_assert_cmpuint(cs_disasm(decoder, mov2, 15, 0x2000, 1,
            &expected, 0, expected_buf.bytes), ==, 1);
        g_assert_cmpmem(actual->detail, sizeof(cs_detail),
                        expected->detail, sizeof(cs_detail));

        g_assert_cmpuint(cs_disasm(decoder, rel16, 15, 0x1000, 1,
            &raw_insn, 0, raw.bytes), ==, 1);
        ir1_decode_template_insert(rel16, 15, mode, raw_insn);
        actual = ir1_decode_template_lookup(rel16_next, 15, 0x3000, 1,
                                            output.bytes, mode);
        g_assert_nonnull(actual);
        g_assert_cmpuint(cs_disasm(decoder, rel16_next, 15, 0x3000, 1,
            &expected, 0, expected_buf.bytes), ==, 1);
        g_assert_cmpmem(actual->detail, sizeof(cs_detail),
                        expected->detail, sizeof(cs_detail));
    }

    g_assert_cmpuint(cs_disasm(decoders[2], rip, 15, 0x1000, 1,
        &raw_insn, 0, raw.bytes), ==, 1);
    ir1_decode_template_insert(rip, 15, 64, raw_insn);
    actual = ir1_decode_template_lookup(rip2, 15, 0x2000, 1,
                                        output.bytes, 64);
    g_assert_nonnull(actual);
    g_assert_cmpuint(cs_disasm(decoders[2], rip2, 15, 0x2000, 1,
        &expected, 0, expected_buf.bytes), ==, 1);
    g_assert_cmpmem(actual->bytes, actual->size, rip2, expected->size);
    g_assert_cmpmem(actual->detail, sizeof(cs_detail),
                    expected->detail, sizeof(cs_detail));

    g_assert_cmpuint(ir1_decode_template_relative_imm_mask(raw_insn), ==, 0);
}

static gpointer cache_policy_worker(gpointer opaque)
{
    int expected_ways = GPOINTER_TO_INT(opaque);
    DecodeBuffer output = { 0 };
    int present = 0;

    g_assert_null(ir1_decode_tags);
    g_assert_null(ir1_decode_payloads);
    g_assert_null(ir1_decode_template_valid);
    g_assert_null(ir1_decode_template_policy);
    g_assert_cmpuint(ir1_decode_template_entries(), ==, 64);
    g_assert_cmpuint(ir1_decode_template_ways(), ==, expected_ways);
    g_assert_cmpuint(ir1_decode_template_sets(), ==, 64 / expected_ways);

    for (int i = 0; i < expected_ways + 1; i++) {
        uint8_t bytes[15] = { 0xc8, i, 0, i };
        cs_insn *insn = NULL;

        g_assert_cmpuint(cs_disasm(decoders[2], bytes, 15, 0, 1,
            &insn, 0, output.bytes), ==, 1);
        ir1_decode_template_insert(bytes, 15, 64, insn);
    }
    for (int i = 0; i < expected_ways + 1; i++) {
        uint8_t bytes[15] = { 0xc8, i, 0, i };

        clear_fast_cache();
        present += ir1_decode_template_lookup(bytes, 15, 0, 0,
                                              output.bytes, 64) != NULL;
        clear_fast_cache();
    }
    g_assert_cmpint(present, ==, expected_ways);
    return NULL;
}

static void test_cache_policies(void)
{
    const int ways[] = { 1, 2, 4 };
    const int replacements[] = {
        LATX_IR1_CACHE_REPLACE_LRU,
        LATX_IR1_CACHE_REPLACE_FIFO,
        LATX_IR1_CACHE_REPLACE_RANDOM,
    };

    for (size_t w = 0; w < G_N_ELEMENTS(ways); w++) {
        for (size_t r = 0; r < G_N_ELEMENTS(replacements); r++) {
            GThread *thread;

            option_ir1_cache_entries = 64;
            option_ir1_cache_ways = ways[w];
            option_ir1_cache_replace = replacements[r];
            thread = g_thread_new("cache-policy",
                                  cache_policy_worker,
                                  GINT_TO_POINTER(ways[w]));
            g_thread_join(thread);
        }
    }
    option_ir1_cache_entries = 0;
    option_ir1_cache_ways = LATX_IR1_CACHE_WAYS_DEFAULT;
    option_ir1_cache_replace = LATX_IR1_CACHE_REPLACE_LRU;
}

static gpointer fast_cache_entries_worker(gpointer opaque)
{
    DecodeBuffer buffer = { 0 }, output = { 0 };
    uint8_t code[15] = { 0x89, 0xd8 };
    cs_insn *expected = NULL, *actual;

    g_assert_cmpuint(ir1_decode_fast_cache_entries(), ==, 32768);
    g_assert_cmpuint(ir1_decode_fast_cache_sets(), ==, 32768);
    g_assert_cmpuint(cs_disasm(decoders[1], code, 15, 0x1000, 1,
                               &expected, 0, buffer.bytes), ==, 1);
    ir1_decode_template_insert(code, 15, 32, expected);
    actual = ir1_decode_template_lookup(code, 15, 0x2000, 1,
                                        output.bytes, 32);
    g_assert_nonnull(actual);
    g_assert_true(ir1_decode_last_fast_hit);
    return NULL;
}

static void test_fast_cache_entries(void)
{
    GThread *thread;

    option_ir1_cache_entries = 64;
    option_ir1_fast_cache_entries = 32768;
    thread = g_thread_new("fast-cache-32768", fast_cache_entries_worker, NULL);
    g_thread_join(thread);
    option_ir1_fast_cache_entries = LATX_IR1_FAST_CACHE_ENTRIES_DEFAULT;
    option_ir1_cache_entries = 0;
}

static gpointer cache_timing_worker(gpointer opaque)
{
    DecodeBuffer output = { 0 };
    uint8_t code[15] = { 0x0f, 0xaf, 0xc1 };
    Ir1DecodeStats *stats;

    for (int i = 0; i < 2; i++) {
        cs_insn *info = NULL;

        g_assert_cmpuint(latxs_decode_one(decoders[2], code, 0x1000 + i * 0x20,
                                         64, &info, 0, output.bytes), ==, 1);
        g_assert_nonnull(info);
    }
    stats = ir1_decode_stats_get();
    g_assert_cmpuint(stats->cache_miss_ns[2], >, 0);
    g_assert_cmpuint(stats->cache_hit_ns[2], >, 0);
    g_assert_cmpuint(stats->miss_lookup_ns[2], >, 0);
    g_assert_cmpuint(stats->miss_capstone_ns[2], >, 0);
    g_assert_cmpuint(stats->miss_insert_ns[2], >, 0);
    return NULL;
}

static void test_cache_timing(void)
{
    GThread *thread;

    option_ir1_cache_entries = 64;
    option_ir1_cache_ways = 4;
    option_ir1_cache_replace = LATX_IR1_CACHE_REPLACE_LRU;
    option_ir1_timing = 1;
    thread = g_thread_new("cache-timing", cache_timing_worker, NULL);
    g_thread_join(thread);
    option_ir1_cache_entries = 0;
    option_ir1_timing = 0;
}

static gpointer cache_layout_worker(gpointer opaque)
{
    DecodeBuffer buffer = { 0 }, output = { 0 };
    uint8_t code[15] = { 0x89, 0xd8 };
    cs_insn *expected = NULL, *actual;
    Ir1DecodeStats *stats;
    size_t entries = 64;
    size_t sets = entries / 4;

    g_assert_null(ir1_decode_tags);
    g_assert_null(ir1_decode_payloads);
    g_assert_cmpuint(sizeof(Ir1DecodeTag), >=, 40);
    g_assert_cmpuint(sizeof(Ir1DecodeTag), <=, 64);
    g_assert_cmpuint(sizeof(Ir1DecodePayload), >, sizeof(Ir1DecodeTag));
    g_assert_cmpuint(cs_disasm(decoders[1], code, 15, 0x1000, 1,
                               &expected, 0, buffer.bytes), ==, 1);
    ir1_decode_template_insert(code, 15, 32, expected);
    g_assert_nonnull(ir1_decode_tags);
    g_assert_nonnull(ir1_decode_payloads);

    stats = ir1_decode_stats_get();
    g_assert_cmpuint(stats->cache_allocations, ==, 1);
    g_assert_cmpuint(stats->cache_tag_alloc_bytes, ==,
                     sizeof(Ir1DecodeTag) * entries);
    g_assert_cmpuint(stats->cache_payload_alloc_bytes, ==,
                     sizeof(Ir1DecodePayload) * entries);
    g_assert_cmpuint(stats->cache_alloc_bytes, ==,
                     sizeof(Ir1DecodeTag) * entries +
                     sizeof(Ir1DecodePayload) * entries +
                     sizeof(uint8_t) * entries + sizeof(uint8_t) * sets);

    actual = ir1_decode_template_lookup(code, 15, 0x2000, 1,
                                        output.bytes, 32);
    g_assert_nonnull(actual);
    return NULL;
}

static void test_cache_layout(void)
{
    GThread *thread;

    option_ir1_cache_entries = 64;
    option_ir1_cache_ways = 4;
    option_ir1_cache_replace = LATX_IR1_CACHE_REPLACE_LRU;
    option_ir1_profile = 1;
    thread = g_thread_new("cache-layout", cache_layout_worker, NULL);
    g_thread_join(thread);
    option_ir1_cache_entries = 0;
    option_ir1_profile = 0;
}

static void test_defaults(void)
{
    g_assert_cmpuint(latxs_ir1_cache_entries(), ==, 16384);
    g_assert_cmpuint(latxs_ir1_cache_ways(), ==, 4);
    g_assert_cmpint(option_ir1_cache_enabled, ==, 1);
    g_assert_cmpint(option_ir1_cache_mode, ==, LATX_IR1_CACHE_MODE_ON);
    g_assert_cmpstr(latxs_ir1_cache_mode_name(LATX_IR1_CACHE_MODE_OFF),
                    ==, "off");
    g_assert_cmpstr(latxs_ir1_cache_mode_name(LATX_IR1_CACHE_MODE_ON),
                    ==, "on");
    g_assert_cmpstr(latxs_ir1_cache_mode_name(LATX_IR1_CACHE_MODE_MAX),
                    ==, "max");
    g_assert_cmpint(option_hot_decode_enabled, ==, 1);
    g_assert_cmpint(option_ir1_timing, ==, 0);
    g_assert_cmpint(option_ir1_distribution, ==, 0);
    g_assert_cmpint(option_ir1_profile, ==, 0);
    g_assert_cmpint(option_ir1_fast_cache_entries, ==, 4096);
    g_assert_cmpint(option_ir1_hash, ==, LATX_IR1_HASH_MURMUR);
    g_assert_cmpint(option_ir1_cache_replace, ==,
                    LATX_IR1_CACHE_REPLACE_LRU);
}

static gpointer cache_disabled_worker(gpointer opaque)
{
    DecodeBuffer output = { 0 };
    uint8_t code[15] = { 0x89, 0xd8 };
    cs_insn *info = NULL;
    Ir1DecodeStats *stats;

    g_assert_cmpuint(latxs_decode_one(decoders[1], code, 0x1000, 32,
                                      &info, 0, output.bytes), ==, 1);
    g_assert_nonnull(info);
    g_assert_null(ir1_decode_tags);
    g_assert_null(ir1_decode_payloads);
    stats = ir1_decode_stats_get();
    g_assert_cmpuint(stats->cache_lookups[1], ==, 0);
    g_assert_cmpuint(stats->cache_hits[1], ==, 0);
    g_assert_cmpuint(stats->full_success[1], ==, 1);
    g_assert_cmpuint(stats->cache_hit_ns[1], ==, 0);
    g_assert_cmpuint(stats->cache_miss_ns[1], ==, 0);
    g_assert_cmpuint(stats->miss_lookup_ns[1], ==, 0);
    g_assert_cmpuint(stats->miss_capstone_ns[1], ==, 0);
    g_assert_cmpuint(stats->miss_insert_ns[1], ==, 0);
    g_assert_cmpuint(stats->direct_ns[1], ==, 0);
    return NULL;
}

static void test_cache_disabled(void)
{
    GThread *thread;

    option_ir1_cache_entries = 64;
    option_ir1_cache_enabled = 0;
    option_ir1_profile = 1;
    option_hot_decode_enabled = 0;
    thread = g_thread_new("cache-disabled", cache_disabled_worker, NULL);
    g_thread_join(thread);
    option_ir1_cache_enabled = 1;
    option_ir1_profile = 0;
    option_hot_decode_enabled = 1;
    option_ir1_cache_entries = 0;
}

static gpointer cache_disabled_timing_worker(gpointer opaque)
{
    DecodeBuffer output = { 0 };
    uint8_t code[15] = { 0x0f, 0xaf, 0xc1 };
    cs_insn *info = NULL;
    Ir1DecodeStats *stats;

    g_assert_cmpuint(latxs_decode_one(decoders[1], code, 0x2000, 32,
                                      &info, 0, output.bytes), ==, 1);
    g_assert_nonnull(info);
    g_assert_null(ir1_decode_tags);
    g_assert_null(ir1_decode_payloads);
    stats = ir1_decode_stats_get();
    g_assert_cmpuint(stats->cache_lookups[1], ==, 0);
    g_assert_cmpuint(stats->cache_hit_ns[1], ==, 0);
    g_assert_cmpuint(stats->cache_miss_ns[1], ==, 0);
    g_assert_cmpuint(stats->miss_lookup_ns[1], ==, 0);
    g_assert_cmpuint(stats->miss_capstone_ns[1], ==, 0);
    g_assert_cmpuint(stats->miss_insert_ns[1], ==, 0);
    g_assert_cmpuint(stats->direct_ns[1], >, 0);
    return NULL;
}

static void test_cache_disabled_timing(void)
{
    GThread *thread;

    option_ir1_cache_entries = 64;
    option_ir1_cache_enabled = 0;
    option_ir1_timing = 1;
    thread = g_thread_new("cache-disabled-timing",
                          cache_disabled_timing_worker, NULL);
    g_thread_join(thread);
    option_ir1_cache_enabled = 1;
    option_ir1_cache_entries = 0;
    option_ir1_timing = 0;
}

static gpointer timing_disabled_worker(gpointer opaque)
{
    DecodeBuffer output = { 0 };
    uint8_t code[15] = { 0x0f, 0xaf, 0xc1 };
    cs_insn *info = NULL;
    Ir1DecodeStats *stats;

    for (int i = 0; i < 2; i++) {
        g_assert_cmpuint(latxs_decode_one(decoders[2], code,
                                          0x3000 + i * 0x20, 64, &info, 0,
                                          output.bytes), ==, 1);
        g_assert_nonnull(info);
    }
    stats = ir1_decode_stats_get();
    g_assert_cmpuint(stats->cache_lookups[2], ==, 2);
    g_assert_cmpuint(stats->cache_hits[2], ==, 1);
    g_assert_cmpuint(stats->cache_hit_ns[2], ==, 0);
    g_assert_cmpuint(stats->cache_miss_ns[2], ==, 0);
    g_assert_cmpuint(stats->miss_lookup_ns[2], ==, 0);
    g_assert_cmpuint(stats->miss_capstone_ns[2], ==, 0);
    g_assert_cmpuint(stats->miss_insert_ns[2], ==, 0);
    g_assert_cmpuint(stats->direct_ns[2], ==, 0);
    return NULL;
}

static void test_timing_disabled(void)
{
    GThread *thread;

    option_ir1_cache_entries = 64;
    option_ir1_cache_enabled = 1;
    option_ir1_profile = 1;
    option_ir1_timing = 0;
    thread = g_thread_new("timing-disabled", timing_disabled_worker, NULL);
    g_thread_join(thread);
    option_ir1_cache_entries = 0;
    option_ir1_profile = 0;
}

static gpointer profile_disabled_worker(gpointer opaque)
{
    DecodeBuffer output = { 0 };
    uint8_t code[15] = { 0x0f, 0xaf, 0xc1 };
    cs_insn *info = NULL;

    g_assert_null(ir1_decode_stats);
    g_assert_cmpuint(latxs_decode_one(decoders[2], code, 0x5000, 64,
                                      &info, 0, output.bytes), ==, 1);
    g_assert_nonnull(info);
    g_assert_null(ir1_decode_stats);
    return NULL;
}

static void test_profile_disabled(void)
{
    GThread *thread;

    option_ir1_profile = 0;
    option_ir1_timing = 0;
    option_ir1_distribution = 0;
    option_ir1_cache_enabled = 0;
    option_hot_decode_enabled = 0;
    thread = g_thread_new("profile-disabled", profile_disabled_worker, NULL);
    g_thread_join(thread);
    option_ir1_cache_enabled = 1;
    option_hot_decode_enabled = 1;
}

static gpointer profile_enabled_worker(gpointer opaque)
{
    DecodeBuffer output = { 0 };
    uint8_t code[15] = { 0x0f, 0xaf, 0xc1 };
    cs_insn *info = NULL;
    Ir1DecodeStats *stats;

    g_assert_cmpuint(latxs_decode_one(decoders[2], code, 0x6000, 64,
                                      &info, 0, output.bytes), ==, 1);
    g_assert_nonnull(info);
    stats = ir1_decode_stats_get();
    g_assert_nonnull(stats);
    g_assert_cmpuint(stats->calls[2], ==, 1);
    g_assert_cmpuint(stats->full_success[2], ==, 1);
    return NULL;
}

static void test_profile_enabled(void)
{
    GThread *thread;

    option_ir1_profile = 1;
    option_ir1_timing = 0;
    option_ir1_distribution = 0;
    option_ir1_cache_enabled = 0;
    option_hot_decode_enabled = 0;
    thread = g_thread_new("profile-enabled", profile_enabled_worker, NULL);
    g_thread_join(thread);
    option_ir1_profile = 0;
    option_ir1_cache_enabled = 1;
    option_hot_decode_enabled = 1;
}

static gpointer decode_order_worker(gpointer opaque)
{
    DecodeBuffer expected_buffer = { 0 }, output = { 0 };
    uint8_t code[15] = { 0x89, 0xd8 };
    cs_insn *expected = NULL, *actual = NULL;
    Ir1DecodeStats *stats;

    (void)opaque;
    option_hot_decode_enabled = 1;
    option_ir1_cache_enabled = 1;
    g_assert_cmpuint(cs_disasm(decoders[1], code, 15, 0x7000, 1,
                               &expected, 0, expected_buffer.bytes), ==, 1);
    ir1_decode_template_insert(code, 15, 32, expected);
    g_assert_cmpuint(latxs_decode_one(decoders[1], code, 0x7000, 32,
                                      &actual, 0, output.bytes), ==, 1);
    g_assert_nonnull(actual);
    stats = ir1_decode_stats_get();
    g_assert_cmpuint(stats->hot_hits[1], ==, 1);
    g_assert_cmpuint(stats->cache_lookups[1], ==, 0);
    g_assert_cmpuint(stats->cache_hits[1], ==, 0);
    g_assert_cmpuint(stats->fast_cache_hits[1], ==, 0);
    g_assert_false(ir1_decode_last_fast_hit);
    return NULL;
}

static void test_decode_order(void)
{
    GThread *thread;

    option_ir1_profile = 1;
    option_ir1_timing = 0;
    option_ir1_distribution = 0;
    thread = g_thread_new("default-order", decode_order_worker, NULL);
    g_thread_join(thread);
    option_ir1_profile = 0;
}

enum HotTimingCase {
    HOT_TIMING_DISABLED_HOT,
    HOT_TIMING_ENABLED,
    HOT_TIMING_DISABLED_TIMER,
};

static gpointer hot_timing_worker(gpointer opaque)
{
    enum HotTimingCase test = GPOINTER_TO_INT(opaque);
    DecodeBuffer output = { 0 };
    uint8_t hit[15] = { 0x89, 0xd8 };
    uint8_t miss[15] = { 0x0f, 0xaf, 0xc1 };
    cs_insn *info = NULL;
    Ir1DecodeStats *stats;

    g_assert_cmpuint(latxs_decode_one(decoders[1], hit, 0x1000, 32,
                                      &info, 0, output.bytes), ==, 1);
    g_assert_nonnull(info);
    g_assert_cmpuint(latxs_decode_one(decoders[1], miss, 0x1020, 32,
                                      &info, 0, output.bytes), ==, 1);
    g_assert_nonnull(info);

    stats = ir1_decode_stats_get();
    g_assert_cmpuint(stats->calls[1], ==, 2);
    g_assert_cmpuint(stats->successes[1], ==, 2);
    if (test == HOT_TIMING_DISABLED_HOT) {
        g_assert_cmpuint(stats->hot_hits[1], ==, 0);
        g_assert_cmpuint(stats->hot_attempt_samples[1], ==, 0);
        g_assert_cmpuint(stats->hot_success_samples[1], ==, 0);
        g_assert_cmpuint(stats->hot_failure_samples[1], ==, 0);
        g_assert_cmpuint(stats->hot_attempt_ns[1], ==, 0);
        g_assert_cmpuint(stats->hot_success_ns[1], ==, 0);
        g_assert_cmpuint(stats->hot_failure_ns[1], ==, 0);
    } else {
        g_assert_cmpuint(stats->hot_hits[1], ==, 1);
        if (test == HOT_TIMING_ENABLED) {
            g_assert_cmpuint(stats->hot_attempt_samples[1], ==, 2);
            g_assert_cmpuint(stats->hot_success_samples[1], ==, 1);
            g_assert_cmpuint(stats->hot_failure_samples[1], ==, 1);
            g_assert_cmpuint(stats->hot_attempt_ns[1], >, 0);
            g_assert_cmpuint(stats->hot_success_ns[1], >, 0);
            g_assert_cmpuint(stats->hot_failure_ns[1], >, 0);
        } else {
            g_assert_cmpuint(stats->hot_attempt_samples[1], ==, 0);
            g_assert_cmpuint(stats->hot_success_samples[1], ==, 0);
            g_assert_cmpuint(stats->hot_failure_samples[1], ==, 0);
            g_assert_cmpuint(stats->hot_attempt_ns[1], ==, 0);
            g_assert_cmpuint(stats->hot_success_ns[1], ==, 0);
            g_assert_cmpuint(stats->hot_failure_ns[1], ==, 0);
        }
    }
    return NULL;
}

static void test_hot_timing(void)
{
    const enum HotTimingCase cases[] = {
        HOT_TIMING_DISABLED_HOT,
        HOT_TIMING_ENABLED,
        HOT_TIMING_DISABLED_TIMER,
    };

    option_ir1_cache_entries = 0;
    option_ir1_cache_enabled = 0;
    option_ir1_profile = 1;
    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        GThread *thread;

        option_hot_decode_enabled = cases[i] == HOT_TIMING_DISABLED_HOT ? 0 : 1;
        option_ir1_timing = cases[i] == HOT_TIMING_ENABLED;
        thread = g_thread_new("hot-timing", hot_timing_worker,
                              GINT_TO_POINTER(cases[i]));
        g_thread_join(thread);
    }
    option_hot_decode_enabled = 1;
    option_ir1_cache_enabled = 1;
    option_ir1_profile = 0;
    option_ir1_timing = 0;
}

static gpointer cache_worker(gpointer opaque)
{
    const cs_insn *expected = opaque;
    DecodeBuffer output = { 0 };
    g_assert_null(ir1_decode_tags);
    g_assert_null(ir1_decode_payloads);
    g_assert_null(ir1_decode_template_valid);
    g_assert_null(ir1_decode_template_policy);
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
    const uint8_t incsspd[15] = { 0xf3, 0x0f, 0xae, 0xe8 };
    const uint8_t incsspq[15] = { 0xf3, 0x48, 0x0f, 0xae, 0xeb };
    DecodeBuffer out = { 0 };
    cs_insn *insn = NULL;
    g_assert_cmpuint(latxs_decode_one(decoders[1], incsspd, 0x1000, 32,
                                      &insn, 0, out.bytes), ==, 0);
    g_assert_null(insn);
    g_assert_null(ir1_decode_template_lookup(incsspd, 15, 0x1000, 0,
                                             out.bytes, 32));
    g_assert_cmpuint(latxs_decode_one(decoders[2], incsspd, 0x1000, 64,
                                      &insn, 0, out.bytes), ==, 0);
    g_assert_null(insn);
    g_assert_cmpuint(latxs_decode_one(decoders[2], incsspq, 0x1000, 64,
                                      &insn, 0, out.bytes), ==, 0);
    g_assert_null(insn);
    g_assert_null(ir1_decode_template_lookup(incsspq, 15, 0x1000, 0,
                                             out.bytes, 64));
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
                compare_mode(code, UINT64_C(0x7fff12345678), 64);
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
                compare_mode(code, UINT64_MAX - 4, 64);
            }
        }
    }
    for (int op = 0; op < 256; op++) {
        for (int modrm = 0; modrm < 256; modrm++) {
            memset(code, modrm & 1 ? 0x81 : 0x7e, sizeof(code));
            code[0] = op;
            code[1] = modrm;
            compare_mode(code, UINT64_C(0x76543210), 32);
        }
    }
    for (int op = 0; op < 256; op++) {
        for (int mod = 0; mod < 3; mod++) {
            for (int sib = 0; sib < 256; sib++) {
                memset(code, 0x80, sizeof(code));
                code[0] = op;
                code[1] = (mod << 6) | 4;
                code[2] = sib;
                compare_mode(code, UINT64_C(0xfffffff0), 32);
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
        compare_mode(code, sample & 1 ? 0 : UINT64_MAX - 4, 64);
    }
    random = 0x9abcdef0;
    for (int sample = 0; sample < 250000; sample++) {
        for (int i = 0; i < 15; i++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            code[i] = random;
        }
        compare_mode(code, sample & 1 ? 0 : UINT64_C(0xfffffff0), 32);
    }
    test_hot_decode_32();
    test_hot_rejections_32();
    test_hash_strategies();
    test_cache();
    test_distribution();
    test_cache_main_fallback();
    test_cache_relocation();
    test_cache_policies();
    test_fast_cache_entries();
    test_cache_timing();
    test_cache_layout();
    test_defaults();
    test_cache_disabled();
    test_cache_disabled_timing();
    test_timing_disabled();
    test_profile_disabled();
    test_profile_enabled();
    test_decode_order();
    test_hot_timing();
    test_threads();
    test_frontend();
    g_assert_cmpuint(checked, >, 250000);
    printf("fast decoder checked=%u field mismatches=%u\n", checked, failures);
    return failures ? 1 : 0;
}
