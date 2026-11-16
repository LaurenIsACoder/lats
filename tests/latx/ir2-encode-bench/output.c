/* SPDX-License-Identifier: GPL-2.0-only */
#include "output-support.h"

__thread ENV *lsenv;
int option_new_assemble, option_dump_host, option_dump, option_split_search;
int in_aot_translate;
enum { MEMBERS = 4, INSNS = 128, CAPACITY = 16384, SEARCH_SIZE = 4096 };
static ENV env;
static TRANSLATION_DATA td;
static TranslationBlock blocks[MEMBERS], *list[MEMBERS];
static struct separated_data data[MEMBERS];
static IR1_INST guest[MEMBERS][INSNS];
static cs_insn insns[MEMBERS][INSNS];
static IR2_INST input[CAPACITY];
static uint32_t code[CAPACITY];
static uint8_t searches[MEMBERS][SEARCH_SIZE];
static unsigned members, guest_count, real_count, label_calls, dump_calls;
static unsigned bytes[MEMBERS], words[MEMBERS];
static int label_error, alloc_fail, filter_matches;
static uint32_t rng;

void latxs_label_dispose(const void *buffer)
{
    (void)buffer;
    label_calls++;
}

int output_labels_tu(TranslationBlock **tbs)
{
    label_calls++;
    td.curr_tb = tbs[members - 1]; /* Real label processing finishes on last TB. */
    return label_error;
}

bool qemu_log_in_addr_range(uint64_t addr)
{
    (void)addr;
    return filter_matches;
}

int latxs_ir2_dump(IR2_INST *in)
{
    dump_calls++;
    fprintf(stderr, "dump opcode=%d\n", in->_opcode);
    return 0;
}

void *region_alloc_search(TranslationBlock *tb, uint32_t mtcc)
{
    (void)mtcc;
    ptrdiff_t index = tb - blocks;
    g_assert(index >= 0 && index < MEMBERS);
    return index == alloc_fail ? NULL : searches[index];
}

void print_buffer_size(uint32_t mtcc)
{
    (void)mtcc;
}

static uint32_t random_word(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static IR2_INST *append(unsigned opcode)
{
    g_assert_cmpint(td.ir2_cur_nr, <, CAPACITY);
    unsigned id = td.ir2_cur_nr++;
    IR2_INST *in = &input[id];
    in->_opcode = opcode;
    in->_id = id;
    return in;
}

/* Generate already-resolved branches and label markers. Label resolution and
 * search-buffer allocation are unchanged production stages, supplied as hooks.
 * Both encoders, output loops, fix_tb_info and search encoding are real code. */
static void fixture(unsigned seed, unsigned count, unsigned per_guest,
                    bool tu, bool prologue)
{
    memset(&td, 0, sizeof(td));
    memset(blocks, 0, sizeof(blocks));
    memset(data, 0, sizeof(data));
    memset(guest, 0, sizeof(guest));
    memset(insns, 0, sizeof(insns));
    memset(input, 0, sizeof(input));
    memset(code, 0xa5, sizeof(code));
    memset(searches, 0, sizeof(searches));
    memset(words, 0, sizeof(words));
    memset(bytes, 0, sizeof(bytes));
    label_calls = dump_calls = real_count = 0;
    label_error = 0;
    alloc_fail = -1;
    filter_matches = 1;
    rng = seed + 1;
    members = tu ? 1 + seed % MEMBERS : 1;
    guest_count = prologue ? 0 : count;
    td.ir2_array = input;
    td.ir1_inst_array = guest[0];
    td.ir1_nr = guest_count;
    td.code_highwater = code + CAPACITY;
    for (unsigned t = 0; t < members; t++) {
        list[t] = &blocks[t];
        blocks[t].s_data = &data[t];
        data[t].ir1 = guest[t];
        blocks[t].tc.ptr = code;
        blocks[t].pc = UINT64_C(0x400000) + t * 0x1000 + ((seed & 1) ? 0xff0 : 0);
        blocks[t].cflags = seed & 2 ? CF_PCREL : 0;
        unsigned address = 0;
        for (unsigned i = 0; i < count; i++) {
            insns[t][i].address = blocks[t].pc + address;
            insns[t][i].size = 1 + random_word() % 15;
            address += insns[t][i].size;
            guest[t][i].info = &insns[t][i];
            if (!prologue) {
                append(LISA_X86_INST)->_opnd[0].val = X86_INS_NOP;
            }
            if (!(i % 3)) {
                IR2_INST *label = append(LISA_LABEL);
                label->_opnd[0].val = ++td.label_num;
                label->_b_label_name = "test-label";
            }
#ifdef CONFIG_LATX_PROFILER
            append(LISA_PROFILE);
#endif
            static const unsigned ops[] = {LISA_ADD_D, LISA_LD_D, LISA_BEQ,
                                          LISA_B, LISA_DUP, LISA_ORI};
            unsigned n = per_guest ? per_guest : random_word() % 8;
            for (unsigned j = 0; j < n; j++) {
                IR2_INST *in = append(ops[random_word() % G_N_ELEMENTS(ops)]);
                for (unsigned k = 0; k < 4; k++) {
                    in->_opnd[k]._type = IR2_OPND_IMMD;
                    in->_opnd[k].val = random_word() & 31;
                }
                words[t]++;
                real_count++;
            }
        }
        bytes[t] = address;
        if (t + 1 < members) {
            append(LISA_TU_SPLIT);
        }
    }
    td.curr_tb = prologue ? NULL : &blocks[0];
}

typedef struct Snapshot {
    TRANSLATION_DATA td;
    TranslationBlock blocks[MEMBERS];
    struct separated_data data[MEMBERS];
    uint32_t code[CAPACITY];
    uint8_t searches[MEMBERS][SEARCH_SIZE];
    uint8_t flat[SEARCH_SIZE * MEMBERS];
    uint32_t offsets[MEMBERS];
    int result, flat_size;
    unsigned label_calls, dump_calls;
} Snapshot;
static Snapshot reference, actual;
static uint64_t checked;

static char *capture(bool newer, bool tu, Snapshot *snapshot)
{
    char *text = NULL;
    size_t size = 0;
    FILE *saved = stderr;
    stderr = open_memstream(&text, &size);
    g_assert_nonnull(stderr);
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->result = tu ? (newer ? new_tu(list) : base_tu(list)) :
                           (newer ? new_tb(code) : base_tb(code));
    fclose(stderr);
    stderr = saved;
    memcpy(&snapshot->td, &td, sizeof(td));
    memcpy(snapshot->blocks, blocks, sizeof(blocks));
    memcpy(snapshot->data, data, sizeof(data));
    memcpy(snapshot->code, code, sizeof(code));
    memcpy(snapshot->searches, searches, sizeof(searches));
    if (tu && !option_split_search) {
        snapshot->flat_size = newer ? new_search(snapshot->flat, snapshot->offsets) :
                                     base_search(snapshot->flat, snapshot->offsets);
    }
    snapshot->label_calls = label_calls;
    snapshot->dump_calls = dump_calls;
    if (snapshot->result >= 0) {
        g_assert_cmpint(snapshot->result, ==, real_count);
        for (unsigned t = 0; guest_count && t < members; t++) {
            g_assert_cmpint(blocks[t].icount, ==, guest_count);
            g_assert_cmpint(blocks[t].size, ==, bytes[t]);
            if (tu) {
                g_assert_cmpint(blocks[t].ir2_num, ==, words[t]);
            }
        }
    }
    return text;
}

static void setup(unsigned seed, bool tu, unsigned scenario)
{
    fixture(seed, 1 + seed % 12, 0, tu, !tu && scenario == 5);
    option_new_assemble = (seed >> 2) & 1;
    option_dump_host = (seed >> 3) & 1;
    option_split_search = (seed >> 4) & 1;
    option_dump = 0;
    in_aot_translate = 0;
    switch (scenario) {
    case 1: filter_matches = 0; break;
    case 2: td.code_highwater = code; break;
    case 3: if (tu) { label_error = TU_ASSEMBLE_RETRY_HRAS_LONG_CHECK; } break;
    case 4: if (tu) { option_split_search = 1; alloc_fail = seed % members; } break;
    }
}

static void verify(void)
{
    for (unsigned seed = 0; seed < 256; seed++) {
        for (unsigned tu = 0; tu < 2; tu++) {
            for (unsigned scenario = 0; scenario < 6; scenario++) {
                setup(seed, tu, scenario);
                char *old_log = capture(false, tu, &reference);
                setup(seed, tu, scenario);
                char *new_log = capture(true, tu, &actual);
                if (memcmp(&reference, &actual, sizeof(reference)) || strcmp(old_log, new_log)) {
                    fprintf(stderr, "FAIL seed=%u tu=%u scenario=%u return=%d/%d\n",
                            seed, tu, scenario, reference.result, actual.result);
                    exit(1);
                }
                g_free(old_log);
                g_free(new_log);
                checked++;
            }
        }
    }
    printf("VERIFY output_cases=%" PRIu64 " machine_code=PASS translation_state=PASS "
           "search_records=PASS dump_output=PASS profile=%d\n", checked,
#ifdef CONFIG_LATX_PROFILER
           1
#else
           0
#endif
    );
}

static uint64_t now_ns(void)
{
    struct timespec t;
    g_assert_cmpint(clock_gettime(CLOCK_MONOTONIC_RAW, &t), ==, 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

int main(int argc, char **argv)
{
    lsenv = &env;
    env.tr_data = &td;
    if (argc == 2 && !strcmp(argv[1], "verify")) {
        verify();
        return 0;
    }
    if (argc != 4 || (strcmp(argv[1], "base") && strcmp(argv[1], "new")) ||
        (strcmp(argv[2], "tb") && strcmp(argv[2], "tu"))) {
        fprintf(stderr, "usage: output verify OR output base|new tb|tu passes\n");
        return 1;
    }
    char *end;
    errno = 0;
    unsigned long passes = strtoul(argv[3], &end, 10);
    if (errno || *end || !passes || passes > 10000000) {
        return 1;
    }
    bool tu = !strcmp(argv[2], "tu");
    fixture(3, tu ? 32 : 128, 8, tu, false);
    option_new_assemble = option_split_search = 1;
    option_dump_host = option_dump = 0;
    uint64_t elapsed = 0;
    int result = 0;
    for (unsigned phase = 0; phase < 2; phase++) {
        uint64_t start = now_ns();
        for (unsigned long pass = 0; pass < (phase ? passes : 16); pass++) {
            td.curr_tb = &blocks[0];
            td.ir1_inst_array = guest[0];
            result = tu ? (!strcmp(argv[1], "base") ? base_tu(list) : new_tu(list)) :
                          (!strcmp(argv[1], "base") ? base_tb(code) : new_tb(code));
        }
        elapsed = now_ns() - start;
    }
    g_assert_cmpint(result, ==, real_count);
    uint64_t checksum = 0;
    for (unsigned i = 0; i < real_count; i++) {
        checksum = checksum * 33 + code[i];
    }
    uint64_t operations = (uint64_t)passes * real_count;
    printf("RESULT variant=%s corpus=%s operations=%" PRIu64 " elapsed_ns=%" PRIu64
           " ns_per_op=%.6f million_ops_per_s=%.6f checksum=%016" PRIx64 " PASS\n",
           argv[1], argv[2], operations, elapsed, (double)elapsed / operations,
           (double)operations * 1000 / elapsed, checksum);
    return 0;
}
