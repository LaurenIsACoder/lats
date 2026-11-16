/* SPDX-License-Identifier: GPL-2.0-only */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "target/i386/latx/include/latx-options.h"
#include "target/i386/latx/ir1/ir1-decode.c"

int option_ir1_cache_ways = LATX_IR1_CACHE_WAYS_DEFAULT;
int option_ir1_cache_replace = LATX_IR1_CACHE_REPLACE_LRU;
int option_ir1_cache_enabled = 1;
uint64_t option_ir1_cache_entries = LATX_IR1_CACHE_ENTRIES_DEFAULT;
int option_ir1_timing;
int option_ir1_distribution;
int option_ir1_profile = 1;
int option_ir1_fast_cache_entries = LATX_IR1_FAST_CACHE_ENTRIES_DEFAULT;
int option_ir1_hash = LATX_IR1_HASH_MURMUR;
int option_hot_decode_enabled = 1;

#define BENCH_WARMUP 100000U
#define BENCH_ITERATIONS 1000000U

typedef union DecodeBuffer {
    uint64_t align;
    uint8_t bytes[2 * IR1_INST_SIZE];
} DecodeBuffer;

typedef struct BenchCase {
    const char *name;
    const char *class_name;
    uint8_t code[15];
    uint8_t size;
    int mode;
} BenchCase;

typedef struct BenchResult {
    double hot_tick_ns;
    double hot_mono_ns;
    double fast_lookup_tick_ns;
    double fast_lookup_mono_ns;
    double fast_emit_tick_ns;
    double fast_emit_mono_ns;
    double latxs_hot_on_tick_ns;
    double latxs_hot_on_mono_ns;
    double latxs_hot_off_tick_ns;
    double latxs_hot_off_mono_ns;
    uint64_t checksum;
} BenchResult;

static csh decoders[3];
static volatile uint64_t benchmark_sink;

static uint64_t host_ns(void)
{
    return (uint64_t)cpu_get_host_ticks() * 10;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        abort();
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static uint64_t consume_insn(const cs_insn *insn)
{
    uint64_t value = insn->id ^ insn->address ^ insn->size;

    if (insn->detail && insn->detail->x86.op_count) {
        value ^= insn->detail->x86.operands[0].imm;
    }
    benchmark_sink = benchmark_sink * UINT64_C(0x9e3779b185ebca87) +
                     value + UINT64_C(0x100000001b3);
    return value;
}

static void assert_equivalent(const BenchCase *tc, const cs_insn *actual,
                              const cs_insn *expected)
{
    g_assert_nonnull(actual);
    g_assert_nonnull(expected);
    g_assert_cmpuint(actual->id, ==, expected->id);
    g_assert_cmpuint(actual->size, ==, expected->size);
    g_assert_cmpuint(actual->address, ==, expected->address);
    g_assert_cmpmem(actual->bytes, actual->size, expected->bytes,
                    expected->size);
    g_assert_cmpmem(actual->detail, sizeof(cs_detail),
                    expected->detail, sizeof(cs_detail));
    g_assert_nonnull(tc->name);
}

static __attribute__((noinline)) cs_insn *
call_hot_decode(const uint8_t *code, uint64_t address, void *output, int mode)
{
    return ir1_hot_decode(code, address, 0, output, mode);
}

static __attribute__((noinline)) cs_insn *
call_fast_lookup(const uint8_t *code, uint64_t address, void *output, int mode,
                 Ir1DecodeStats *stats, int mode_index)
{
    return ir1_decode_template_lookup_impl(code, 15, address, 0, output, mode,
                                            stats, mode_index);
}

static __attribute__((noinline)) cs_insn *
call_fast_emit(const Ir1DecodeFastEntry *fast, const uint8_t *code,
               const uint64_t words[2], uint64_t address, void *output,
               int mode, Ir1DecodeStats *stats, int mode_index)
{
    return ir1_decode_fast_template_emit(fast, code, words, address, 0, output,
                                         mode, stats, mode_index);
}

static __attribute__((noinline)) size_t
call_latxs_decode(csh decoder, const uint8_t *code, uint64_t address, int mode,
                  cs_insn **insn, void *output)
{
    return latxs_decode_one(decoder, code, address, mode, insn, 0, output);
}

static uint64_t case_address(const BenchCase *tc)
{
    return tc->mode == 64 ? UINT64_C(0x7fff12345678) :
                            UINT64_C(0x12345678);
}

static gpointer benchmark_case_worker(gpointer opaque)
{
    const BenchCase *tc = opaque;
    BenchResult *result = g_new0(BenchResult, 1);
    DecodeBuffer reference_buffer = { 0 };
    DecodeBuffer output = { 0 };
    Ir1DecodeStats *stats;
    cs_insn *reference = NULL;
    cs_insn *actual = NULL;
    Ir1DecodeFastEntry *fast_entry = NULL;
    uint64_t address = case_address(tc);
    uint64_t code_words[2];
    int mode_index = ir1_decode_stats_mode_index(tc->mode);
    uint32_t hash_tag;
    size_t fast_set;
    uint64_t start;
    uint64_t mono_start;

    g_assert_cmpuint(cs_disasm(decoders[tc->mode == 32 ? 1 : 2], tc->code, 15,
                               address, 1, &reference, 1,
                               reference_buffer.bytes), ==, 1);
    g_assert_cmpuint(mode_index, >=, 0);

    ir1_decode_template_insert(tc->code, 15, tc->mode, reference);
    stats = ir1_decode_stats_get();

    actual = call_fast_lookup(tc->code, address, output.bytes, tc->mode,
                              stats, mode_index);
    g_assert_true(ir1_decode_last_fast_hit);
    assert_equivalent(tc, actual, reference);

    hash_tag = ir1_decode_template_hash_value(tc->code, 15, tc->mode);
    ir1_decode_template_load_words(tc->code, code_words);
    fast_set = hash_tag & (ir1_decode_fast_cache_sets() - 1);
    for (size_t way = 0; way < IR1_DECODE_FAST_CACHE_WAYS; way++) {
        Ir1DecodeFastEntry *candidate =
            &ir1_decode_fast_templates[fast_set * IR1_DECODE_FAST_CACHE_WAYS +
                                       way];

        if (ir1_decode_fast_entry_matches(candidate, code_words, hash_tag,
                                          tc->mode, 15)) {
            fast_entry = candidate;
            break;
        }
    }
    g_assert_nonnull(fast_entry);

    for (unsigned i = 0; i < BENCH_WARMUP; i++) {
        consume_insn(call_hot_decode(tc->code, address, output.bytes, tc->mode));
        consume_insn(call_fast_lookup(tc->code, address, output.bytes, tc->mode,
                                      stats, mode_index));
        consume_insn(call_fast_emit(fast_entry, tc->code, code_words, address,
                                    output.bytes, tc->mode, stats, mode_index));
    }

    start = host_ns();
    mono_start = monotonic_ns();
    for (unsigned i = 0; i < BENCH_ITERATIONS; i++) {
        consume_insn(call_hot_decode(tc->code, address, output.bytes, tc->mode));
    }
    result->hot_mono_ns = (monotonic_ns() - mono_start) /
                          (double)BENCH_ITERATIONS;
    result->hot_tick_ns = (host_ns() - start) / (double)BENCH_ITERATIONS;

    start = host_ns();
    mono_start = monotonic_ns();
    for (unsigned i = 0; i < BENCH_ITERATIONS; i++) {
        consume_insn(call_fast_lookup(tc->code, address, output.bytes, tc->mode,
                                      stats, mode_index));
    }
    result->fast_lookup_mono_ns = (monotonic_ns() - mono_start) /
                                  (double)BENCH_ITERATIONS;
    result->fast_lookup_tick_ns = (host_ns() - start) /
                                  (double)BENCH_ITERATIONS;

    start = host_ns();
    mono_start = monotonic_ns();
    for (unsigned i = 0; i < BENCH_ITERATIONS; i++) {
        consume_insn(call_fast_emit(fast_entry, tc->code, code_words, address,
                                    output.bytes, tc->mode, stats, mode_index));
    }
    result->fast_emit_mono_ns = (monotonic_ns() - mono_start) /
                                (double)BENCH_ITERATIONS;
    result->fast_emit_tick_ns = (host_ns() - start) /
                                (double)BENCH_ITERATIONS;

    option_hot_decode_enabled = 1;
    g_assert_cmpuint(call_latxs_decode(decoders[tc->mode == 32 ? 1 : 2],
                                       tc->code, address, tc->mode, &actual,
                                       output.bytes), ==, 1);
    assert_equivalent(tc, actual, reference);
    for (unsigned i = 0; i < BENCH_WARMUP; i++) {
        g_assert_cmpuint(call_latxs_decode(decoders[tc->mode == 32 ? 1 : 2],
                                           tc->code, address, tc->mode, &actual,
                                           output.bytes), ==, 1);
        consume_insn(actual);
    }
    start = host_ns();
    mono_start = monotonic_ns();
    for (unsigned i = 0; i < BENCH_ITERATIONS; i++) {
        if (call_latxs_decode(decoders[tc->mode == 32 ? 1 : 2], tc->code,
                              address, tc->mode, &actual, output.bytes) != 1) {
            abort();
        }
        consume_insn(actual);
    }
    result->latxs_hot_on_mono_ns = (monotonic_ns() - mono_start) /
                                   (double)BENCH_ITERATIONS;
    result->latxs_hot_on_tick_ns = (host_ns() - start) /
                                   (double)BENCH_ITERATIONS;

    option_hot_decode_enabled = 0;
    for (unsigned i = 0; i < BENCH_WARMUP; i++) {
        g_assert_cmpuint(call_latxs_decode(decoders[tc->mode == 32 ? 1 : 2],
                                           tc->code, address, tc->mode, &actual,
                                           output.bytes), ==, 1);
        consume_insn(actual);
    }
    start = host_ns();
    mono_start = monotonic_ns();
    for (unsigned i = 0; i < BENCH_ITERATIONS; i++) {
        if (call_latxs_decode(decoders[tc->mode == 32 ? 1 : 2], tc->code,
                              address, tc->mode, &actual, output.bytes) != 1) {
            abort();
        }
        consume_insn(actual);
    }
    result->latxs_hot_off_mono_ns = (monotonic_ns() - mono_start) /
                                    (double)BENCH_ITERATIONS;
    result->latxs_hot_off_tick_ns = (host_ns() - start) /
                                    (double)BENCH_ITERATIONS;
    result->checksum = benchmark_sink;
    return result;
}

static void bytes_to_hex(const uint8_t *bytes, int size, char *output)
{
    static const char digits[] = "0123456789abcdef";

    for (int i = 0; i < size; i++) {
        output[i * 2] = digits[bytes[i] >> 4];
        output[i * 2 + 1] = digits[bytes[i] & 0xf];
    }
    output[size * 2] = '\0';
}

int main(void)
{
    const BenchCase cases[] = {
        { "add32_rr", "add", { 0x01, 0xd8 }, 2, 32 },
        { "add64_rr", "add", { 0x48, 0x01, 0xd8 }, 3, 64 },
        { "add32_imm8", "add", { 0x83, 0xc0, 0x7f }, 3, 32 },
        { "add64_imm8", "add", { 0x48, 0x83, 0xc0, 0x7f }, 4, 64 },
        { "jcc32_rel8", "jcc", { 0x74, 0x10 }, 2, 32 },
        { "jcc64_rel8", "jcc", { 0x74, 0x10 }, 2, 64 },
        { "jcc32_rel32", "jcc", { 0x0f, 0x84, 0x10, 0x00, 0x00, 0x00 },
          6, 32 },
        { "jcc64_rel32", "jcc", { 0x0f, 0x84, 0x10, 0x00, 0x00, 0x00 },
          6, 64 },
        { "mov32_rr", "mov", { 0x89, 0xd8 }, 2, 32 },
        { "mov64_rr", "mov", { 0x48, 0x89, 0xd8 }, 3, 64 },
        { "mov32_imm32", "mov", { 0xb8, 0x78, 0x56, 0x34, 0x12 },
          5, 32 },
        { "mov64_imm64", "mov", { 0x48, 0xb8, 0x78, 0x56, 0x34, 0x12,
                                    0xf0, 0xde, 0xbc, 0x9a }, 10, 64 },
        { "lea64_sib", "lea", { 0x48, 0x8d, 0x44, 0x8b, 0x10 }, 5, 64 },
        { "test64_rr", "test", { 0x48, 0x85, 0xd8 }, 3, 64 },
        { "cmp64_rr", "cmp", { 0x48, 0x39, 0xd8 }, 3, 64 },
    };
    const cs_mode modes[] = { CS_MODE_16, CS_MODE_32, CS_MODE_64 };

    for (int i = 0; i < 3; i++) {
        g_assert_cmpint(cs_open(CS_ARCH_X86, modes[i], &decoders[i]), ==,
                        CS_ERR_OK);
        cs_option(decoders[i], CS_OPT_DETAIL, CS_OPT_ON);
    }

    printf("BENCH\tcase\tclass\tmode\tbytes\thot_tick_ns\thot_mono_ns"
           "\tfast_lookup_tick_ns\tfast_lookup_mono_ns"
           "\tfast_emit_tick_ns\tfast_emit_mono_ns"
           "\tlatxs_hot_on_tick_ns\tlatxs_hot_on_mono_ns"
           "\tlatxs_hot_off_tick_ns\tlatxs_hot_off_mono_ns\tchecksum\n");

    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        BenchResult *result;
        GThread *thread = g_thread_new(cases[i].name, benchmark_case_worker,
                                       (gpointer)&cases[i]);
        char bytes[32];
        bytes_to_hex(cases[i].code, cases[i].size, bytes);
        result = g_thread_join(thread);
        printf("BENCH\t%s\t%s\t%d\t%s\t%.3f\t%.3f\t%.3f\t%.3f"
               "\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t0x%016" PRIx64 "\n",
               cases[i].name, cases[i].class_name, cases[i].mode, bytes,
               result->hot_tick_ns, result->hot_mono_ns,
               result->fast_lookup_tick_ns, result->fast_lookup_mono_ns,
               result->fast_emit_tick_ns, result->fast_emit_mono_ns,
               result->latxs_hot_on_tick_ns, result->latxs_hot_on_mono_ns,
               result->latxs_hot_off_tick_ns, result->latxs_hot_off_mono_ns,
               result->checksum);
        g_free(result);
    }

    for (int i = 0; i < 3; i++) {
        cs_close(&decoders[i]);
    }
    return 0;
}
