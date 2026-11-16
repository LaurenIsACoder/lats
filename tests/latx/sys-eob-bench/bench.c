/* SPDX-License-Identifier: GPL-2.0-only */
/* Compare actual production functions and structures; no translator launcher. */
#include "qemu/osdep.h"
#include "cpu.h"
#include "lsenv.h"
#include "latx-options.h"

int base_latxs_ir1_is_eob_in_sys(IR1_INST *);
int new_latxs_ir1_is_eob_in_sys(IR1_INST *);
__thread ENV *lsenv;
__thread CPUState *current_cpu;
int option_pause;
int option_inhibit_irq;
int option_split_strins;

static CPUState cpu;
static ENV env;
static TRANSLATION_DATA td, initial, expected;
static CPUBreakpoint breakpoint;
static CPUWatchpoint watchpoint;
static uint64_t checked;

/* Each scenario changes a condition that can end a block or modify its flags. */
static void context(IR1_INST *in, unsigned mode, unsigned scenario, unsigned seed)
{
    memset(&td, 0, sizeof(td));
    td.sys.pe = mode & 1;
    td.sys.vm86 = (mode >> 1) & 1;
    td.sys.code32 = (mode >> 2) & 1;
    td.sys.cs_base = (mode & 1) ? UINT64_C(0xffff800000000000) : 0;
    td.sys.pc = in->info->address + td.sys.cs_base;
    td.can_link = td.can_indir_link = seed;
    td.sys_eob_can_link = td.sys_eob_is_indir = td.sys_eob_use_fsht = seed;
    td.sys.bp_hit = seed;
    option_pause = 1;
    option_inhibit_irq = 1;
    option_split_strins = 0;
    cpu.singlestep_enabled = 0;
    cpu.watchpoint_hit = NULL;
    QTAILQ_INIT(&cpu.breakpoints);
    switch (scenario) {
    case 1: td.sys.tf = 1; break;
    case 2: td.sys.flags = HF_INHIBIT_IRQ_MASK; break;
    case 3: td.sys.cflags = CF_USE_ICOUNT; break;
    case 4:
        td.sys.cflags = CF_USE_ICOUNT;
        in->info->detail->x86.opcode[0] = 0xa4;
        in->info->detail->x86.prefix[0] = X86_PREFIX_REP;
        break;
    case 5:
        option_split_strins = 1;
        in->info->detail->x86.opcode[0] = 0xa4;
        in->info->detail->x86.prefix[0] = X86_PREFIX_REPNE;
        break;
    case 6: in->info->address |= 0xfff; break;
    case 7:
        in->info->address = (in->info->address & TARGET_PAGE_MASK) | 0xffd;
        in->info->size = 3;
        break;
    case 8: cpu.singlestep_enabled = 1; break;
    case 9:
    case 10:
        breakpoint.pc = in->info->address + td.sys.cs_base + (scenario == 10);
        QTAILQ_INSERT_TAIL(&cpu.breakpoints, &breakpoint, entry);
        break;
    case 11: cpu.watchpoint_hit = &watchpoint; break;
    case 12: option_pause = 0; break;
    case 13:
        option_inhibit_irq = 0;
        td.sys.flags = HF_INHIBIT_IRQ_MASK;
        break;
    case 14:
        td.sys.tf = 1;
        td.sys.cflags = CF_USE_ICOUNT;
        option_pause = 0;
        cpu.watchpoint_hit = &watchpoint;
        break;
    case 15:
        td.sys.cflags = CF_USE_ICOUNT;
        in->info->address |= 0xfff;
        break;
    }
}

static void check(IR1_INST *in, unsigned mode, unsigned scenario, unsigned seed)
{
    cs_insn saved_insn = *in->info;
    cs_detail saved_detail = *in->info->detail;
    context(in, mode, scenario, seed);
    memcpy(&initial, &td, sizeof(td));
    int want = base_latxs_ir1_is_eob_in_sys(in);
    memcpy(&expected, &td, sizeof(td));
    memcpy(&td, &initial, sizeof(td));
    int got = new_latxs_ir1_is_eob_in_sys(in);
    if (want != got || memcmp(&expected, &td, sizeof(td))) {
        fprintf(stderr, "FAIL opcode=%u mode=%u scenario=%u seed=%u "
                "return=%d/%d operands=%d:%u,%d:%u\n", in->info->id,
                mode, scenario, seed, want, got,
                ir1_get_opnd(in, 0)->type, ir1_get_opnd(in, 0)->reg,
                ir1_get_opnd(in, 1)->type, ir1_get_opnd(in, 1)->reg);
        exit(1);
    }
    checked++;
    *in->info = saved_insn;
    *in->info->detail = saved_detail;
}

static void matrix(IR1_INST *in)
{
    for (unsigned mode = 0; mode < 8; mode++) {
        for (unsigned scenario = 0; scenario < 16; scenario++) {
            for (unsigned seed = 0; seed < 2; seed++) {
                check(in, mode, scenario, seed);
            }
        }
    }
}

static void verify_synthetic(void)
{
    cs_detail detail = { 0 };
    cs_insn insn = { .address = 0x400000, .size = 3, .detail = &detail };
    IR1_INST in = { .info = &insn };
    detail.x86.opcode[0] = 0x90;
    detail.x86.op_count = 2;
    detail.x86.operands[0].type = X86_OP_REG;
    detail.x86.operands[0].reg = X86_REG_EAX;
    detail.x86.operands[1].type = X86_OP_REG;
    detail.x86.operands[1].reg = X86_REG_ECX;
    for (unsigned op = X86_INS_INVALID; op < X86_INS_ENDING; op++) {
        insn.id = op;
        matrix(&in);
    }
    for (unsigned pop = 0; pop < 2; pop++) {
        insn.id = pop ? X86_INS_POP : X86_INS_MOV;
        for (unsigned operand = 0; operand < 2; operand++) {
            for (unsigned reg = X86_REG_INVALID; reg < X86_REG_ENDING; reg++) {
                for (unsigned memory = 0; memory < 2; memory++) {
                    cs_x86_op saved = detail.x86.operands[operand];
                    detail.x86.operands[operand].type = memory ? X86_OP_MEM : X86_OP_REG;
                    detail.x86.operands[operand].reg = reg;
                    matrix(&in);
                    detail.x86.operands[operand] = saved;
                }
            }
        }
    }
    /* NOP's raw F3 byte is independent of Capstone's prefix metadata. */
    insn.id = X86_INS_NOP;
    insn.bytes[0] = X86_PREFIX_REPE;
    matrix(&in);
}

static void expect(IR1_INST *in, unsigned mode, unsigned scenario,
                   int result, int can_link, int sys_link, int bp_hit)
{
    cs_insn insn = *in->info;
    cs_detail detail = *in->info->detail;
    context(in, mode, scenario, 0);
    td.can_link = td.can_indir_link = 1;
    g_assert_cmpint(new_latxs_ir1_is_eob_in_sys(in), ==, result);
    g_assert_cmpint(td.can_link, ==, can_link);
    g_assert_cmpint(td.can_indir_link, ==, can_link);
    g_assert_cmpint(td.sys_eob_can_link, ==, sys_link);
    g_assert_cmpint(td.sys.bp_hit, ==, bp_hit);
    *in->info = insn;
    *in->info->detail = detail;
}

/* Independent expectations catch loss of short-circuit state changes. */
static void verify_expected(void)
{
    cs_detail detail = { 0 };
    cs_insn insn = { .address = 0x400000, .size = 3, .detail = &detail };
    IR1_INST in = { .info = &insn };
    detail.x86.op_count = 2;
    detail.x86.operands[0].type = X86_OP_REG;
    detail.x86.operands[0].reg = X86_REG_EAX;
    detail.x86.operands[1].type = X86_OP_REG;
    detail.x86.operands[1].reg = X86_REG_ECX;
    insn.id = X86_INS_MOV;
    expect(&in, 0, 1, 1, 0, 0, 0);  /* TF disables links. */
    expect(&in, 0, 8, 1, 1, 0, 0);  /* CPU single step leaves bp_hit clear. */
    expect(&in, 0, 9, 1, 1, 0, 1);  /* Matching breakpoint. */
    expect(&in, 0, 11, 1, 1, 0, 1); /* Watchpoint. */
    detail.x86.operands[0].reg = X86_REG_CR8;
    expect(&in, 0, 0, 1, 1, 1, 0);
    expect(&in, 0, 3, 1, 1, 0, 0);  /* Icount only links MOV from CR. */
    detail.x86.operands[0].reg = X86_REG_EAX;
    detail.x86.operands[1].reg = X86_REG_CR3;
    expect(&in, 0, 3, 1, 1, 1, 0);
    expect(&in, 0, 0, 0, 1, 0, 0);
    detail.x86.operands[1].reg = X86_REG_ECX;
    detail.x86.operands[0].reg = X86_REG_SS;
    expect(&in, 0, 1, 1, 1, 0, 0);  /* MOV SS ends before TF processing. */
    expect(&in, 0, 13, 0, 1, 0, 0); /* Inhibit option disabled. */
    detail.x86.operands[0].reg = X86_REG_DS;
    expect(&in, 5, 0, 1, 1, 0, 0);
    expect(&in, 7, 0, 0, 1, 0, 0);  /* vm86 suppresses protected-mode rule. */
    insn.id = X86_INS_STI;
    expect(&in, 0, 1, 1, 1, 0, 0);
    insn.id = X86_INS_PAUSE;
    expect(&in, 0, 12, 1, 1, 0, 0);
    expect(&in, 0, 14, 1, 0, 0, 0); /* TF before PAUSE before watchpoint. */
    insn.id = X86_INS_NOP;
    insn.bytes[0] = X86_PREFIX_REPE;
    expect(&in, 0, 12, 1, 1, 0, 0);
    const unsigned barriers[] = {X86_INS_INVPCID, X86_INS_LDMXCSR,
        X86_INS_FXRSTOR, X86_INS_FXRSTOR64, X86_INS_XRSTOR, X86_INS_XRSTOR64};
    for (unsigned i = 0; i < G_N_ELEMENTS(barriers); i++) {
        insn.id = barriers[i];
        expect(&in, 0, 1, 1, 1, 0, 0); /* Barrier returns before TF. */
    }
}

typedef struct Sample {
    uint8_t size;
    uint8_t bytes[15];
} Sample;

static const Sample samples[] = {
    {3, {0x48, 0x89, 0xd8}}, {3, {0x48, 0x8b, 0x07}},
    {3, {0x48, 0x89, 0x07}}, {4, {0x48, 0x83, 0xc0, 1}},
    {3, {0x48, 0x39, 0xc8}}, {2, {0x75, 0x10}},
    {4, {0x48, 0x8d, 0x47, 8}}, {3, {0x48, 0x31, 0xc0}},
    {1, {0x50}}, {1, {0x58}}, {1, {0xc3}}, {2, {0xff, 0xd0}},
    {5, {0xe8, 0x10, 0, 0, 0}}, {2, {0x85, 0xc0}},
    {1, {0x90}}, {2, {0xf3, 0x90}}, {2, {0xf3, 0xa4}},
    {3, {0x0f, 0x20, 0xc0}}, {3, {0x0f, 0x22, 0xc0}},
    {4, {0x44, 0x0f, 0x22, 0xc0}}, {3, {0x0f, 0x23, 0xc0}},
    {2, {0x8e, 0xd8}}, {2, {0x8e, 0xd0}}, {1, {0x17}},
    {1, {0xfb}}, {1, {0x9d}}, {2, {0x0f, 0x05}},
    {2, {0x0f, 0x07}}, {1, {0xcf}}, {3, {0x0f, 0x01, 0x38}},
    {5, {0x66, 0x0f, 0x38, 0x82, 0x00}},
    {3, {0x0f, 0xae, 0x10}}, {3, {0x0f, 0xae, 0x08}},
    {4, {0x48, 0x0f, 0xae, 0x08}}, {3, {0x0f, 0xae, 0x28}},
    {4, {0x48, 0x0f, 0xae, 0x28}}, {3, {0x0f, 0x01, 0xd1}},
    {3, {0x0f, 0x01, 0xca}}, {3, {0x0f, 0x01, 0xcb}}
};

static bool decode(csh decoder, const Sample *sample, IR1_INST *in)
{
    cs_insn *insn;
    void *storage = g_malloc0(IR1_INST_SIZE);
    if (cs_disasm(decoder, sample->bytes, sample->size, 0x400000, 1,
                  &insn, 0, storage) != 1) {
        g_free(storage);
        return false;
    }
    in->info = insn;
    return true;
}

static void verify(void)
{
    verify_expected();
    verify_synthetic();
    const cs_mode modes[] = {CS_MODE_16, CS_MODE_32, CS_MODE_64};
    unsigned decoded = 0;
    for (unsigned m = 0; m < G_N_ELEMENTS(modes); m++) {
        csh decoder;
        g_assert_cmpint(cs_open(CS_ARCH_X86, modes[m], &decoder), ==, CS_ERR_OK);
        cs_option(decoder, CS_OPT_DETAIL, CS_OPT_ON);
        for (unsigned i = 0; i < G_N_ELEMENTS(samples); i++) {
            IR1_INST in = { 0 };
            if (decode(decoder, &samples[i], &in)) {
                matrix(&in);
                decoded++;
                g_free(in.info);
            }
        }
        cs_close(&decoder);
    }
    printf("VERIFY comparisons=%" PRIu64 " decoded_samples=%u "
           "opcodes=%u registers=%u full_translation_state=PASS\n",
           checked, decoded, X86_INS_ENDING, X86_REG_ENDING);
}

enum { CORPUS_SIZE = 4096 };
static IR1_INST corpus[CORPUS_SIZE];
static int output[CORPUS_SIZE];

#define LOOP(name) \
static void run_##name(unsigned passes) \
{ \
    for (unsigned pass = 0; pass < passes; pass++) { \
        for (unsigned i = 0; i < CORPUS_SIZE; i++) { \
            output[i] = name##_latxs_ir1_is_eob_in_sys(&corpus[i]); \
        } \
    } \
}
LOOP(base)
LOOP(new)

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
    current_cpu = &cpu;
    if (argc == 2 && !strcmp(argv[1], "verify")) {
        verify();
        return 0;
    }
    if (argc != 4 || (strcmp(argv[1], "base") && strcmp(argv[1], "new")) ||
        (strcmp(argv[2], "hot") && strcmp(argv[2], "mixed"))) {
        fprintf(stderr, "usage: bench verify OR bench base|new hot|mixed passes\n");
        return 1;
    }
    char *end;
    errno = 0;
    unsigned long passes = strtoul(argv[3], &end, 10);
    if (errno || *end || !passes || passes > 10000000) {
        return 1;
    }
    csh decoder;
    g_assert_cmpint(cs_open(CS_ARCH_X86, CS_MODE_64, &decoder), ==, CS_ERR_OK);
    cs_option(decoder, CS_OPT_DETAIL, CS_OPT_ON);
    uint32_t rng = 0x831ad7c4;
    for (unsigned i = 0; i < CORPUS_SIZE; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        unsigned count = !strcmp(argv[2], "hot") ? 15 : G_N_ELEMENTS(samples);
        unsigned sample = rng % count;
        while (!decode(decoder, &samples[sample], &corpus[i])) {
            sample = (sample + 1) % count;
        }
    }
    cs_close(&decoder);
    context(&corpus[0], 0, 0, 0);
    void (*run)(unsigned) = !strcmp(argv[1], "base") ? run_base : run_new;
    run(16);
    uint64_t start = now_ns();
    run(passes);
    uint64_t elapsed = now_ns() - start;
    uint64_t checksum = 0;
    for (unsigned i = 0; i < CORPUS_SIZE; i++) {
        g_assert_cmpint(output[i], ==, base_latxs_ir1_is_eob_in_sys(&corpus[i]));
        checksum = checksum * 33 + output[i];
        g_free(corpus[i].info);
    }
    uint64_t operations = (uint64_t)passes * CORPUS_SIZE;
    printf("RESULT variant=%s corpus=%s operations=%" PRIu64
           " elapsed_ns=%" PRIu64 " ns_per_op=%.6f million_ops_per_s=%.6f "
           "checksum=%016" PRIx64 " PASS\n", argv[1], argv[2], operations,
           elapsed, (double)elapsed / operations, (double)operations * 1000 / elapsed,
           checksum);
    return 0;
}
