/* SPDX-License-Identifier: GPL-2.0-only */
#include "../../target/i386/latx/optimization/sys-ghbr.c"
#include "lsenv.h"
#include "translate.h"
#include "reg-alloc.h"

__thread ENV *lsenv;
IR2_OPND latxs_zero_ir2_opnd = { ._type = IR2_OPND_GPR, .val = 0 };
static IR2_OPCODE emitted;
static unsigned int emitted_count;

IR2_OPND latxs_ra_alloc_gpr(int reg)
{
    IR2_OPND result = { ._type = IR2_OPND_GPR, .val = reg + 4 };
    return result;
}

IR2_INST *latxs_append_ir2_opnd2_(IR2_OPCODE opcode,
                                 IR2_OPND *dest, IR2_OPND *src)
{
    emitted = opcode;
    emitted_count++;
    return NULL;
}

IR2_INST *latxs_append_ir2_opnd3(IR2_OPCODE opcode, IR2_OPND *dest,
                                IR2_OPND *src, IR2_OPND *other)
{
    emitted = opcode;
    emitted_count++;
    return NULL;
}

IR2_INST *latxs_append_ir2_opnd2ii(IR2_OPCODE opcode, IR2_OPND *dest,
                                  IR2_OPND *src, int hi, int lo)
{
    emitted = opcode;
    emitted_count++;
    return NULL;
}

int option_ghbr = LATXS_GHBR_BASIC;
int option_int_opt = IRQ_CHECK_BLOCK_END;
int option_tucp_fast;
static csh decoder;
static unsigned int checks;

static IR1_INST decode(const uint8_t *bytes, size_t size)
{
    IR1_INST ir1 = { 0 };
    cs_insn *insn;
    void *storage = g_malloc0(IR1_INST_SIZE);

    g_assert_cmpuint(cs_disasm(decoder, bytes, size, 0x400000, 1, &insn,
                              0, storage), ==, 1);
    ir1.info = insn;
    return ir1;
}

static void check_fact_masks(const uint8_t *bytes, size_t size,
                             uint16_t use, uint16_t def, uint16_t candidate)
{
    IR1_INST ir1 = decode(bytes, size);
    GhbrFacts facts = ghbr_facts(&ir1);

    g_assert_false(facts.barrier);
    g_assert_cmphex(facts.use, ==, use);
    g_assert_cmphex(facts.def, ==, def);
    g_assert_cmphex(facts.candidate, ==, candidate);
    g_free(ir1.info);
    checks++;
}

static void check_barrier(const uint8_t *bytes, size_t size, int level)
{
    IR1_INST ir1 = decode(bytes, size);
    int saved_level = option_ghbr;

    option_ghbr = level;
    g_assert_true(ghbr_facts(&ir1).barrier);
    option_ghbr = saved_level;
    g_free(ir1.info);
    checks++;
}

static void check_lea_facts(void)
{
#define FACTS(use, def, candidate, ...) do { \
    const uint8_t bytes[] = { __VA_ARGS__ }; \
    check_fact_masks(bytes, sizeof(bytes), use, def, candidate); \
} while (0)
    FACTS(0, 2, 2, 0x8d, 0x08);              /* lea ecx,[rax] */
    FACTS(1, 2, 0, 0x48, 0x8d, 0x08);        /* lea rcx,[rax] */
    FACTS(0, 2, 0, 0x67, 0x48, 0x8d, 0x08); /* lea rcx,[eax] */
#undef FACTS
}

static void check_memory_facts(void)
{
    const uint8_t load[] = { 0x8b, 0x0f };             /* mov ecx,[rdi] */
    const uint8_t store[] = { 0x89, 0x07 };            /* mov [rdi],eax */
    const uint8_t indexed[] = { 0x8b, 0x0c, 0x50 };   /* mov ecx,[rax+rdx*2] */
    const uint8_t addr32[] = { 0x67, 0x8b, 0x08 };     /* mov ecx,[eax] */
    const uint8_t riprel[] = { 0x8b, 0x0d, 0, 0, 0, 0 }; /* mov ecx,[rip] */
    int saved_level = option_ghbr;

    check_barrier(load, sizeof(load), LATXS_GHBR_BASIC);
    check_barrier(store, sizeof(store), LATXS_GHBR_BASIC);
    option_ghbr = LATXS_GHBR_IGNORE_MEMORY;
    check_fact_masks(load, sizeof(load), 1U << edi_index,
                     1U << ecx_index, 1U << ecx_index);
    check_fact_masks(store, sizeof(store), 1U << edi_index, 0, 0);
    check_fact_masks(indexed, sizeof(indexed),
                     (1U << eax_index) | (1U << edx_index),
                     1U << ecx_index, 1U << ecx_index);
    check_fact_masks(addr32, sizeof(addr32), 0,
                     1U << ecx_index, 1U << ecx_index);
    check_fact_masks(riprel, sizeof(riprel), 0,
                     1U << ecx_index, 1U << ecx_index);
    option_ghbr = saved_level;
}

static void init_tb(TranslationBlock *tb, IR1_INST *ir1, int count)
{
    memset(tb, 0, sizeof(*tb));
    tb->s_data = g_new0(struct separated_data, 1);
    tb->s_data->sys_eob_ir1_index = -1;
    tb->s_data->tb_too_large_ir1_index = -1;
    tb->s_data->ir1 = ir1;
    tb->s_data->last_ir1_type = IR1_TYPE_NORMAL;
    tb->flags = HF_CS64_MASK | (3 << HF_CPL_SHIFT);
    tb->pc = 0x400000;
    tb->icount = count;
}

static void check_sequence(const uint8_t *middle, size_t size, bool hit)
{
    const uint8_t add[] = { 0x83, 0xc0, 1 };
    const uint8_t zero[] = { 0x31, 0xc0 };
    IR1_INST ir1[] = { decode(add, sizeof(add)), decode(middle, size),
                       decode(zero, sizeof(zero)) };
    TranslationBlock tb, *list[] = { &tb };
    int saved_level = option_ghbr;

    init_tb(&tb, ir1, ARRAY_SIZE(ir1));
    latxs_ghbr_analyze_tu(1, list);
    g_assert_cmpint(latxs_ghbr_can_omit(&ir1[0], 0), ==, hit);
    g_assert_cmphex(ir1[2].ghbr_opt, ==, 0);
    checks++;
    for (int cpl = 0; cpl < 3; cpl++) {
        tb.flags = HF_CS64_MASK | (cpl << HF_CPL_SHIFT);
        latxs_ghbr_analyze_tu(1, list);
        g_assert_cmphex(ir1[0].ghbr_opt, ==, 0);
        checks++;
    }
    tb.flags = 3 << HF_CPL_SHIFT;
    latxs_ghbr_analyze_tu(1, list);
    g_assert_cmphex(ir1[0].ghbr_opt, ==, 0);
    tb.flags |= HF_CS64_MASK;
    const uint32_t cflags[] = { CF_SINGLE_STEP, CF_USE_ICOUNT, CF_LAST_IO };
    for (int i = 0; i < ARRAY_SIZE(cflags); i++) {
        tb.cflags = cflags[i];
        latxs_ghbr_analyze_tu(1, list);
        g_assert_cmphex(ir1[0].ghbr_opt, ==, 0);
        checks++;
    }
    tb.cflags = 0;
    tb.flags |= HF_TF_MASK;
    latxs_ghbr_analyze_tu(1, list);
    g_assert_cmphex(ir1[0].ghbr_opt, ==, 0);
    tb.flags &= ~HF_TF_MASK;
    option_ghbr = LATXS_GHBR_DISABLED;
    ir1[0].ghbr_opt = GHBR_ALL;
    latxs_ghbr_analyze_tu(1, list);
    g_assert_cmphex(ir1[0].ghbr_opt, ==, 0);
    option_ghbr = saved_level;
    for (int i = 0; i < ARRAY_SIZE(ir1); i++) {
        g_free(ir1[i].info);
    }
    g_free(tb.s_data);
    checks += 3;
}

static void check_level_sequence(const uint8_t *middle, size_t size,
                                 int level, bool hit)
{
    const uint8_t add[] = { 0x83, 0xc0, 1 };      /* add eax,1 */
    const uint8_t zero[] = { 0x31, 0xc0 };        /* xor eax,eax */
    IR1_INST ir1[] = { decode(add, sizeof(add)), decode(middle, size),
                       decode(zero, sizeof(zero)) };
    TranslationBlock tb, *list[] = { &tb };
    int saved_level = option_ghbr;

    init_tb(&tb, ir1, ARRAY_SIZE(ir1));
    option_ghbr = level;
    latxs_ghbr_analyze_tu(1, list);
    g_assert_cmpint(latxs_ghbr_can_omit(&ir1[0], eax_index), ==, hit);
    for (int i = 0; i < ARRAY_SIZE(ir1); i++) {
        g_free(ir1[i].info);
    }
    g_free(tb.s_data);
    option_ghbr = saved_level;
    checks++;
}

static void check_memory_liveness(void)
{
#define LEVEL_SEQUENCE(level, hit, ...) do { \
    const uint8_t bytes[] = { __VA_ARGS__ }; \
    check_level_sequence(bytes, sizeof(bytes), level, hit); \
} while (0)
    LEVEL_SEQUENCE(LATXS_GHBR_BASIC, false,
                   0x8b, 0x0f);                  /* mov ecx,[rdi] */
    LEVEL_SEQUENCE(LATXS_GHBR_BASIC, false,
                   0x89, 0x07);                  /* mov [rdi],eax */
    LEVEL_SEQUENCE(LATXS_GHBR_IGNORE_MEMORY, true,
                   0x8b, 0x0f);                  /* mov ecx,[rdi] */
    LEVEL_SEQUENCE(LATXS_GHBR_IGNORE_MEMORY, true,
                   0x89, 0x07);                  /* mov [rdi],eax */
    LEVEL_SEQUENCE(LATXS_GHBR_IGNORE_MEMORY, false,
                   0x8b, 0x08);                  /* mov ecx,[rax] */
    LEVEL_SEQUENCE(LATXS_GHBR_IGNORE_MEMORY, true,
                   0x67, 0x8b, 0x08);            /* mov ecx,[eax] */
    LEVEL_SEQUENCE(LATXS_GHBR_IGNORE_MEMORY, true,
                   0x0f, 0xbe, 0x07);            /* movsx eax,byte [rdi] */
    LEVEL_SEQUENCE(LATXS_GHBR_IGNORE_MEMORY, true,
                   0x63, 0x07);                  /* movsxd eax,dword [rdi] */
    LEVEL_SEQUENCE(LATXS_GHBR_IGNORE_MEMORY, false,
                   0xf7, 0x37);                  /* div dword ptr [rdi] */
    LEVEL_SEQUENCE(LATXS_GHBR_IGNORE_MEMORY, false,
                   0x0f, 0xa5, 0x0f);            /* shld [rdi],ecx,cl */
#undef LEVEL_SEQUENCE
}

static void check_edges(void)
{
    const uint8_t add[] = { 0x83, 0xc0, 1 };
    const uint8_t jump[] = { 0xeb, 0 };
    const uint8_t zero[] = { 0x31, 0xc0 };
    IR1_INST ai[] = { decode(add, sizeof(add)), decode(jump, sizeof(jump)) };
    IR1_INST bi[] = { decode(zero, sizeof(zero)) };
    TranslationBlock a, b, *list[] = { &a, &b };

    init_tb(&a, ai, ARRAY_SIZE(ai));
    init_tb(&b, bi, ARRAY_SIZE(bi));
    b.pc += 16;
    a.s_data->last_ir1_type = IR1_TYPE_JUMP;
    a.s_data->next_tb[TU_TB_INDEX_TARGET] = &b;
    latxs_ghbr_analyze_tu(2, list);
    g_assert_true(latxs_ghbr_can_omit(&ai[0], 0));
    a.s_data->last_ir1_type = IR1_TYPE_BRANCH;
    latxs_ghbr_analyze_tu(2, list);
    g_assert_false(latxs_ghbr_can_omit(&ai[0], 0));
    a.s_data->last_ir1_type = IR1_TYPE_JUMP;
    a.s_data->sys_eob_ir1_index = 1;
    latxs_ghbr_analyze_tu(2, list);
    g_assert_false(latxs_ghbr_can_omit(&ai[0], 0));
    a.s_data->sys_eob_ir1_index = -1;
    b.pc = a.pc - 16;
    latxs_ghbr_analyze_tu(2, list);
    g_assert_false(latxs_ghbr_can_omit(&ai[0], 0));
    b.pc = a.pc + TARGET_PAGE_SIZE;
    latxs_ghbr_analyze_tu(2, list);
    g_assert_false(latxs_ghbr_can_omit(&ai[0], 0));
    b.pc = a.pc + 16;
    option_int_opt = IRQ_CHECK_BLOCK_START;
    latxs_ghbr_analyze_tu(2, list);
    g_assert_false(latxs_ghbr_can_omit(&ai[0], 0));
    option_int_opt = IRQ_CHECK_BLOCK_END;
    ai[0].instptn.opc = 1;
    latxs_ghbr_analyze_tu(2, list);
    g_assert_false(latxs_ghbr_can_omit(&ai[0], 0));
    for (int i = 0; i < ARRAY_SIZE(ai); i++) {
        g_free(ai[i].info);
    }
    g_free(bi[0].info);
    g_free(a.s_data);
    g_free(b.s_data);
    checks += 7;
}

static void check_bswap_facts(void)
{
    const uint8_t bytes[] = { 0x0f, 0xc8 }; /* bswap eax */
    check_fact_masks(bytes, sizeof(bytes), 0, 1, 1);
}

static void check_shift_facts(void)
{
#define FACTS(expected_use, expected_def, expected_candidate, ...) do { \
    const uint8_t bytes[] = { __VA_ARGS__ }; \
    IR1_INST ir1 = decode(bytes, sizeof(bytes)); \
    GhbrFacts facts = ghbr_facts(&ir1); \
    g_assert_false(facts.barrier); \
    g_assert_cmphex(facts.use, ==, expected_use); \
    g_assert_cmphex(facts.def, ==, expected_def); \
    g_assert_cmphex(facts.candidate, ==, expected_candidate); \
    g_free(ir1.info); \
    checks++; \
} while (0)
    FACTS(0, 1, 1, 0xd3, 0xe8);              /* shr eax,cl */
    FACTS(0, 1, 1, 0xc1, 0xe8, 0x00);        /* shr eax,0 */
    FACTS(0, 1, 1, 0xd3, 0xc8);              /* ror eax,cl */
    FACTS(0, 1, 1, 0x0f, 0x45, 0xc1);       /* cmovne eax,ecx */
#undef FACTS
    const uint8_t shrd[] = { 0x0f, 0xad, 0xc8 }; /* shrd eax,ecx,cl */
    const uint8_t shld[] = { 0x0f, 0xa5, 0xc8 }; /* shld eax,ecx,cl */
    IR1_INST shrd_ir1 = decode(shrd, sizeof(shrd));
    IR1_INST shld_ir1 = decode(shld, sizeof(shld));
    g_assert_true(ghbr_facts(&shrd_ir1).barrier);
    g_assert_true(ghbr_facts(&shld_ir1).barrier);
    g_free(shrd_ir1.info);
    g_free(shld_ir1.info);
    checks += 2;
}

static void check_shift_liveness(void)
{
    const uint8_t add[] = { 0x83, 0xc0, 1 };       /* add eax,1 */
    const uint8_t shift[] = { 0xd3, 0xe8 };        /* shr eax,cl */
    const uint8_t read[] = { 0x48, 0x89, 0xc1 };  /* mov rcx,rax */
    const uint8_t kill[] = { 0x31, 0xc9 };         /* xor ecx,ecx */
    IR1_INST ir1[] = { decode(add, sizeof(add)),
                       decode(shift, sizeof(shift)),
                       decode(read, sizeof(read)),
                       decode(kill, sizeof(kill)) };
    TranslationBlock tb, *list[] = { &tb };

    init_tb(&tb, ir1, ARRAY_SIZE(ir1));
    latxs_ghbr_analyze_tu(1, list);
    g_assert_true(latxs_ghbr_can_omit(&ir1[0], 0));
    g_assert_false(latxs_ghbr_can_omit(&ir1[1], 0));
    for (int i = 0; i < ARRAY_SIZE(ir1); i++) {
        g_free(ir1[i].info);
    }
    g_free(tb.s_data);
    checks += 2;
}

static void check_cmov_liveness(void)
{
    const uint8_t add[] = { 0x83, 0xc0, 1 };       /* add eax,1 */
    const uint8_t cmov32[] = { 0x0f, 0x45, 0xc1 }; /* cmovne eax,ecx */
    const uint8_t cmov64[] = { 0x48, 0x0f, 0x45, 0xc1 }; /* cmovne rax,rcx */
    const uint8_t read[] = { 0x48, 0x89, 0xc2 };   /* mov rdx,rax */
    IR1_INST ir1_32[] = { decode(add, sizeof(add)),
                          decode(cmov32, sizeof(cmov32)),
                          decode(read, sizeof(read)) };
    IR1_INST ir1_64[] = { decode(add, sizeof(add)),
                          decode(cmov64, sizeof(cmov64)),
                          decode(read, sizeof(read)) };
    TranslationBlock tb32, tb64, *list32[] = { &tb32 }, *list64[] = { &tb64 };

    init_tb(&tb32, ir1_32, ARRAY_SIZE(ir1_32));
    init_tb(&tb64, ir1_64, ARRAY_SIZE(ir1_64));
    latxs_ghbr_analyze_tu(1, list32);
    latxs_ghbr_analyze_tu(1, list64);
    g_assert_true(latxs_ghbr_can_omit(&ir1_32[0], eax_index));
    g_assert_false(latxs_ghbr_can_omit(&ir1_64[0], eax_index));
    for (int i = 0; i < ARRAY_SIZE(ir1_32); i++) {
        g_free(ir1_32[i].info);
        g_free(ir1_64[i].info);
    }
    g_free(tb32.s_data);
    g_free(tb64.s_data);
    checks += 2;
}

static void check_writeback(void)
{
    const uint8_t add[] = { 0x83, 0xc0, 1 };
    IR1_INST ir1 = decode(add, sizeof(add));
    TRANSLATION_DATA td = { 0 };
    ENV env = { .tr_data = &td };
    IR2_OPND src = latxs_ra_alloc_gpr(0);

    lsenv = &env;
    td.curr_ir1_inst = &ir1;
    td.is_tu_mode = 1;
    qatomic_set(&ghbr_stats_state, 2);
    qatomic_set(&ghbr_stats.emitted_elisions, 0);
    qatomic_set(&ghbr_stats.discarded_elisions, 0);
    qatomic_set(&ghbr_stats.rollbacks, 0);
    latxs_ghbr_stats_restore(0);
    emitted_count = 0;
    latxs_store_ir2_to_ir1_gpr(&src, ir1_get_opnd(&ir1, 0));
    g_assert_cmpint(emitted, ==, lisa_mov32z);
    g_assert_cmpuint(qatomic_read(&ghbr_stats.emitted_elisions), ==, 0);
    ir1.ghbr_opt = 1;
    emitted_count = 0;
    latxs_store_ir2_to_ir1_gpr(&src, ir1_get_opnd(&ir1, 0));
    g_assert_cmpuint(emitted_count, ==, 0);
    g_assert_cmpuint(latxs_ghbr_stats_checkpoint(), ==, 1);
    src = latxs_ra_alloc_gpr(1);
    latxs_store_ir2_to_ir1_gpr(&src, ir1_get_opnd(&ir1, 0));
    g_assert_cmpuint(emitted_count, ==, 1);
    g_assert_cmpint(emitted, ==, LISA_OR);
    g_assert_cmpuint(latxs_ghbr_stats_checkpoint(), ==, 2);
    td.is_tu_mode = 0;
    latxs_store_ir2_to_ir1_gpr(&src, ir1_get_opnd(&ir1, 0));
    g_assert_cmpint(emitted, ==, lisa_mov32z);
    g_assert_cmpuint(latxs_ghbr_stats_checkpoint(), ==, 2);
    td.is_tu_mode = 1;
    option_ghbr = LATXS_GHBR_DISABLED;
    latxs_store_ir2_to_ir1_gpr(&src, ir1_get_opnd(&ir1, 0));
    g_assert_cmpint(emitted, ==, lisa_mov32z);
    g_assert_cmpuint(latxs_ghbr_stats_checkpoint(), ==, 2);
    option_ghbr = LATXS_GHBR_BASIC;
    ir1.ghbr_opt = 2;
    latxs_store_ir2_to_ir1_gpr(&src, ir1_get_opnd(&ir1, 0));
    g_assert_cmpint(emitted, ==, lisa_mov32z);
    g_assert_cmpuint(latxs_ghbr_stats_checkpoint(), ==, 2);
    g_assert_cmpuint(qatomic_read(&ghbr_stats.emitted_elisions), ==, 0);
    latxs_ghbr_stats_commit();
    g_assert_cmpuint(qatomic_read(&ghbr_stats.emitted_elisions), ==, 2);
    g_assert_cmpuint(latxs_ghbr_stats_checkpoint(), ==, 0);
    g_free(ir1.info);
    lsenv = NULL;
    qatomic_set(&ghbr_stats_state, 1);
    checks += 15;
}

static void check_stats_retries(void)
{
    uint64_t checkpoint;

    qatomic_set(&ghbr_stats_state, 2);
    qatomic_set(&ghbr_stats.emitted_elisions, 0);
    latxs_ghbr_stats_restore(0);
    ghbr_note_emitted_elision();
    checkpoint = latxs_ghbr_stats_checkpoint();
    /* Discard a TB attempt while retaining the preceding TB's count. */
    ghbr_note_emitted_elision();
    ghbr_note_emitted_elision();
    latxs_ghbr_stats_restore(checkpoint);
    g_assert_cmpuint(qatomic_read(&ghbr_stats.discarded_elisions), ==, 2);
    g_assert_cmpuint(qatomic_read(&ghbr_stats.rollbacks), ==, 1);
    ghbr_note_emitted_elision();
    latxs_ghbr_stats_commit();
    g_assert_cmpuint(qatomic_read(&ghbr_stats.emitted_elisions), ==, 2);
    /* A whole-TU retry or failure discards every pending operation. */
    ghbr_note_emitted_elision();
    latxs_ghbr_stats_restore(0);
    g_assert_cmpuint(qatomic_read(&ghbr_stats.discarded_elisions), ==, 3);
    g_assert_cmpuint(qatomic_read(&ghbr_stats.rollbacks), ==, 2);
    latxs_ghbr_stats_commit();
    g_assert_cmpuint(qatomic_read(&ghbr_stats.emitted_elisions), ==, 2);
    /* One guest write may remove two host clearing operations. */
    ghbr_note_emitted_elision();
    ghbr_note_emitted_elision();
    latxs_ghbr_stats_commit();
    latxs_ghbr_stats_commit();
    g_assert_cmpuint(qatomic_read(&ghbr_stats.emitted_elisions), ==, 4);
    qatomic_set(&ghbr_stats_state, 1);
    ghbr_note_emitted_elision();
    latxs_ghbr_stats_commit();
    g_assert_cmpuint(qatomic_read(&ghbr_stats.emitted_elisions), ==, 4);
    checks += 8;
}

int main(void)
{
    g_assert_cmpint(cs_open(CS_ARCH_X86, CS_MODE_64, &decoder), ==, CS_ERR_OK);
    cs_option(decoder, CS_OPT_DETAIL, CS_OPT_ON);
    check_lea_facts();
    check_memory_facts();
#define SEQUENCE(hit, ...) do { \
    const uint8_t bytes[] = { __VA_ARGS__ }; \
    check_sequence(bytes, sizeof(bytes), hit); \
} while (0)
    SEQUENCE(true, 0x89, 0xc1);           /* mov ecx,eax */
    SEQUENCE(false, 0x48, 0x89, 0xc1);    /* mov rcx,rax */
    SEQUENCE(true, 0x66, 0x83, 0xc0, 1); /* add ax,1 */
    SEQUENCE(true, 0x0f, 0x45, 0xc1);    /* cmovne eax,ecx */
    SEQUENCE(false, 0x48, 0x0f, 0x45, 0xc1); /* cmovne rax,rcx */
    SEQUENCE(false, 0x8b, 0x0f);         /* mov ecx,[rdi] */
    SEQUENCE(false, 0x89, 0x07);         /* mov [rdi],eax */
    SEQUENCE(false, 0x48, 0x8d, 0x08);   /* lea rcx,[rax] */
    SEQUENCE(false, 0xf7, 0xf1);         /* div ecx */
    SEQUENCE(false, 0xf7, 0xe1);         /* mul ecx */
    SEQUENCE(false, 0x0f, 0xa2);         /* cpuid */
    SEQUENCE(false, 0x0f, 0x05);         /* syscall */
    SEQUENCE(false, 0x50);               /* push rax */
    SEQUENCE(false, 0x0f, 0xbc, 0xc1);   /* bsf eax,ecx */
    SEQUENCE(false, 0x0f, 0xb1, 0xcb);   /* cmpxchg ebx,ecx */
    SEQUENCE(true, 0xd3, 0xe8);          /* shr eax,cl */
    SEQUENCE(false, 0x48, 0xd3, 0xe8);   /* shr rax,cl */
    SEQUENCE(true, 0x8d, 0x08);          /* lea ecx,[rax] */
    SEQUENCE(true, 0x67, 0x48, 0x8d, 0x08); /* lea rcx,[eax] */
    SEQUENCE(false, 0x48, 0x8d, 0x0c, 0x40); /* lea rcx,[rax+rax*2] */
    check_bswap_facts();
    SEQUENCE(true, 0x0f, 0xc8);          /* bswap eax */
    SEQUENCE(false, 0x48, 0x0f, 0xc8);  /* bswap rax */
    check_shift_facts();
    SEQUENCE(true, 0xd3, 0xc8);          /* ror eax,cl */
    SEQUENCE(false, 0x48, 0xd3, 0xc8);   /* ror rax,cl */
#undef SEQUENCE
    check_edges();
    check_shift_liveness();
    check_cmov_liveness();
    check_memory_liveness();
    check_writeback();
    check_stats_retries();
    cs_close(&decoder);
    printf("GHBR regression checks: %u passed\n", checks);
    return 0;
}
