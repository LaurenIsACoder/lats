/* SPDX-License-Identifier: GPL-2.0-only */
/* Compile with the softmmu target flags and --gc-sections (see runner). */
#include "../../target/i386/latx/optimization/sys-shbr.c"

int option_shbr;
int option_sigfpe;
int option_aot_save;
int in_aot_translate;
int option_int_opt;
int option_tucp_fast;

static csh decoder;
static unsigned int checks;

static struct separated_data *new_tb_data(void)
{
    struct separated_data *data = g_malloc0(sizeof(*data));

    data->sys_eob_ir1_index = -1;
    data->tb_too_large_ir1_index = -1;
    return data;
}

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

static void check_facts(const uint8_t *bytes, size_t size,
                        bool memory, bool barrier)
{
    IR1_INST ir1 = decode(bytes, size);

    for (option_shbr = 1; option_shbr <= 3; option_shbr++) {
        for (int width = 32; width <= 64; width += 32) {
            ShbrFacts facts = shbr_get_facts(&ir1, width);
            uint16_t expected = barrier || (memory && option_shbr == 1) ?
                                SHBR_XMM_ALL : 0;

            g_assert_cmphex(facts.read, ==, expected);
            g_assert_cmphex(facts.candidate, ==, 0);
            checks++;
        }
    }
    g_free(ir1.info);
}

static void check_integer_rules(void)
{
#define FACTS(memory, barrier, ...) do { \
    const uint8_t bytes[] = { __VA_ARGS__ }; \
    check_facts(bytes, sizeof(bytes), memory, barrier); \
} while (0)
    FACTS(false, false, 0x48, 0x83, 0xc0, 1); /* add rax,1 */
    FACTS(false, false, 0x48, 0x39, 0xc8);    /* cmp rax,rcx */
    FACTS(false, false, 0x48, 0x89, 0xd8);    /* mov rax,rbx */
    FACTS(false, false, 0x48, 0x8d, 0x47, 8); /* lea rax,[rdi+8] */
    FACTS(true, false, 0x48, 0x8b, 0x07);    /* mov rax,[rdi] */
    FACTS(true, false, 0x48, 0x89, 0x07);    /* mov [rdi],rax */
    FACTS(false, true, 0x0f, 0x20, 0xc0);    /* mov rax,cr0 */
    FACTS(false, true, 0x8e, 0xd8);          /* mov ds,ax */
    FACTS(false, true, 0x48, 0xf7, 0xf1);    /* div rcx */
    FACTS(false, true, 0x0f, 0x05);          /* syscall */
    FACTS(false, true, 0x0f, 0xa2);          /* cpuid */
    FACTS(true, true, 0x0f, 0xae, 0x07);     /* fxsave [rdi] */
    FACTS(false, true, 0xd9, 0xe8);          /* fld1 */
    FACTS(true, true, 0xf0, 0x48, 0x83, 0x07, 1); /* lock add [rdi],1 */
    FACTS(false, false, 0x83, 0xc0, 1);      /* add eax,1 */
    FACTS(false, false, 0x66, 0x83, 0xc0, 1); /* add ax,1 */
    FACTS(false, false, 0x04, 1);            /* add al,1 */
    FACTS(false, false, 0x48, 0x11, 0xc8);   /* adc rax,rcx */
    FACTS(false, false, 0x48, 0x29, 0xc8);   /* sub rax,rcx */
    FACTS(false, false, 0x48, 0x19, 0xc8);   /* sbb rax,rcx */
    FACTS(false, false, 0x48, 0xff, 0xc0);   /* inc rax */
    FACTS(false, false, 0x48, 0xff, 0xc8);   /* dec rax */
    FACTS(false, false, 0x48, 0xf7, 0xd8);   /* neg rax */
    FACTS(false, false, 0x48, 0x21, 0xc8);   /* and rax,rcx */
    FACTS(false, false, 0x48, 0x09, 0xc8);   /* or rax,rcx */
    FACTS(false, false, 0x48, 0x31, 0xc8);   /* xor rax,rcx */
    FACTS(false, false, 0x48, 0x85, 0xc8);   /* test rax,rcx */
    FACTS(false, false, 0x48, 0xf7, 0xd0);   /* not rax */
    FACTS(false, false, 0x48, 0x0f, 0xbe, 0xc0); /* movsx rax,al */
    FACTS(false, false, 0x48, 0x0f, 0xb6, 0xc0); /* movzx rax,al */
    FACTS(false, false, 0x48, 0x63, 0xc0);   /* movsxd rax,eax */
    FACTS(false, false, 0x48, 0xb8, 1, 0, 0, 0, 0, 0, 0, 0); /* movabs */
    FACTS(false, true, 0x0f, 0x21, 0xc0);    /* mov rax,dr0 */
#undef FACTS
}

static void check_run(bool copy, bool unknown_copy, int level, int cpl)
{
    const uint8_t load[] = { 0xf2, 0x0f, 0x10, 0x07 };
    const uint8_t mul[] = { 0xf2, 0x0f, 0x59, 0xc1 };
    const uint8_t store[] = { 0x66, 0x0f, 0x11, 0x06 };
    const uint8_t move[] = { 0x66, 0x0f, 0x28, 0xd0 };
    const uint8_t unknown[] = { 0x66, 0x0f, 0x10, 0x17 };
    const uint8_t mul2[] = { 0xf2, 0x0f, 0x59, 0xd1 };
    const uint8_t add2[] = { 0xf2, 0x0f, 0x58, 0xd1 };
    const uint8_t store2[] = { 0x66, 0x0f, 0x11, 0x16 };
    IR1_INST ir1[5] = { 0 };
    TranslationBlock tb = { 0 };

    ir1[0] = decode(load, sizeof(load));
    if (copy) {
        ir1[1] = unknown_copy ? decode(unknown, sizeof(unknown)) :
                               decode(move, sizeof(move));
        ir1[2] = decode(mul2, sizeof(mul2));
        ir1[3] = decode(add2, sizeof(add2));
        ir1[4] = decode(store2, sizeof(store2));
    } else {
        ir1[1] = decode(mul, sizeof(mul));
        ir1[2] = decode(store, sizeof(store));
    }
    tb.s_data = new_tb_data();
    tb.s_data->ir1 = ir1;
    tb.icount = copy ? 5 : 3;
    tb.flags = HF_OSFXSR_MASK | (cpl << HF_CPL_SHIFT);
    option_shbr = level;
    TranslationBlock *list[] = { &tb };

    shbr_analyze_known_zero_tu(1, list, UINT32_MAX, false);
    if (!copy) {
        g_assert_true(LATXS_SHBR_ON_64(&ir1[0]));
        g_assert_true(LATXS_SHBR_ON_64(&ir1[1]));
        g_assert_cmphex(ir1[2].shbr_restore_zero64_before, ==, 1);
    } else {
        g_assert_cmpint(LATXS_SHBR_ON_64(&ir1[2]), ==, !unknown_copy);
        g_assert_cmpint(LATXS_SHBR_ON_64(&ir1[3]), ==, !unknown_copy);
        g_assert_cmphex(ir1[4].shbr_restore_zero64_before, ==,
                        unknown_copy ? 0 : 4);
    }
    checks++;
    for (int i = 0; i < tb.icount; i++) {
        g_free(ir1[i].info);
    }
    g_free(tb.s_data);
}

static void check_integer_liveness(void)
{
    const uint8_t mul[] = { 0xf2, 0x0f, 0x59, 0xc1 };
    const uint8_t add[] = { 0x48, 0x83, 0xc0, 1 };
    const uint8_t load[] = { 0xf2, 0x0f, 0x10, 0x07 };
    IR1_INST ir1[] = { decode(mul, sizeof(mul)), decode(add, sizeof(add)),
                       decode(load, sizeof(load)) };
    TranslationBlock tb = { 0 };

    tb.s_data = new_tb_data();
    tb.s_data->ir1 = ir1;
    tb.icount = 3;
    tb.flags = HF_OSFXSR_MASK;
    for (option_shbr = 1; option_shbr <= 3; option_shbr++) {
        ir1[0].shbr_flags = 0;
        shbr_scan_block(&tb, SHBR_XMM_ALL, 64, true, UINT32_MAX, false, false);
        g_assert_cmpint(LATXS_SHBR_ON_64(&ir1[0]), ==, option_shbr >= 2);
        checks++;
    }
    option_shbr = 2;
    const unsigned int blocked_flags[] = { HF_TS_MASK, HF_EM_MASK, HF_TF_MASK };
    for (int i = 0; i < ARRAY_SIZE(blocked_flags); i++) {
        tb.flags = HF_OSFXSR_MASK | blocked_flags[i];
        ir1[0].shbr_flags = 0;
        g_assert_cmphex(shbr_scan_block(&tb, 0, 64, true, UINT32_MAX,
                                       false, false), ==, SHBR_XMM_ALL);
        g_assert_false(LATXS_SHBR_ON_64(&ir1[0]));
        checks++;
    }
    tb.flags = HF_OSFXSR_MASK | HF_SIGFPE_ON_MASK;
    option_sigfpe = 1;
    ir1[0].shbr_flags = 0;
    shbr_analyze_known_zero_tb(&tb, UINT32_MAX, false);
    g_assert_false(LATXS_SHBR_ON_64(&ir1[0]));
    option_sigfpe = 0;
    checks++;
    for (int i = 0; i < 3; i++) {
        g_free(ir1[i].info);
    }
    g_free(tb.s_data);
}

static void check_implicit_zero_clobber(bool explicit_length, int level)
{
    const uint8_t zero[] = { 0x66, 0x0f, 0xef, 0xc0 };
    uint8_t mask[] = { 0x66, 0x0f, 0x3a, 0x62, 0xca, 0x40 };
    const uint8_t mul[] = { 0xf2, 0x0f, 0x59, 0xc3 };
    const uint8_t add[] = { 0xf2, 0x0f, 0x58, 0xc3 };
    const uint8_t store[] = { 0x66, 0x0f, 0x11, 0x07 };
    TranslationBlock tb = { 0 };

    if (explicit_length) {
        mask[3] = 0x60; /* PCMPESTRM instead of PCMPISTRM. */
    }
    IR1_INST ir1[] = { decode(zero, sizeof(zero)), decode(mask, sizeof(mask)),
        decode(mul, sizeof(mul)), decode(add, sizeof(add)),
        decode(store, sizeof(store)) };
    tb.s_data = new_tb_data();
    tb.s_data->ir1 = ir1;
    tb.icount = ARRAY_SIZE(ir1);
    tb.flags = HF_OSFXSR_MASK;
    option_shbr = level;
    shbr_analyze_known_zero_tb(&tb, UINT32_MAX, false);
    g_assert_false(LATXS_SHBR_ON_64(&ir1[2]));
    g_assert_false(LATXS_SHBR_ON_64(&ir1[3]));
    g_assert_cmphex(ir1[4].shbr_restore_zero64_before, ==, 0);
    checks++;
    for (int i = 0; i < ARRAY_SIZE(ir1); i++) {
        g_free(ir1[i].info);
    }
    g_free(tb.s_data);
}

static void check_recovery_dispatcher_entry(bool self_loop, int level)
{
    const uint8_t zero[] = { 0x66, 0x0f, 0xef, 0xc0 };
    const uint8_t mul[] = { 0xf2, 0x0f, 0x59, 0xc3 };
    const uint8_t add[] = { 0xf2, 0x0f, 0x58, 0xc3 };
    const uint8_t jump[] = { 0xeb, 0 };
    IR1_INST ai[] = { decode(zero, sizeof(zero)), decode(mul, sizeof(mul)),
        decode(add, sizeof(add)), decode(jump, sizeof(jump)) };
    IR1_INST bi[] = { decode(jump, sizeof(jump)) };
    TranslationBlock a = { 0 }, b = { 0 };
    TranslationBlock *list[] = { &a, &b };

    a.s_data = new_tb_data();
    b.s_data = new_tb_data();
    a.s_data->ir1 = ai;
    b.s_data->ir1 = bi;
    a.icount = ARRAY_SIZE(ai);
    b.icount = ARRAY_SIZE(bi);
    a.pc = 0x400000;
    b.pc = 0x400020;
    a.flags = b.flags = HF_OSFXSR_MASK | (3 << HF_CPL_SHIFT);
    a.s_data->last_ir1_type = b.s_data->last_ir1_type = IR1_TYPE_JUMP;
    a.s_data->next_tb[TU_TB_INDEX_TARGET] = self_loop ? &a : &b;
    b.s_data->next_tb[TU_TB_INDEX_TARGET] = &a;
    option_int_opt = IRQ_CHECK_BLOCK_END;
    option_shbr = level;
    latxs_shbr_analyze_tu(self_loop ? 1 : 2, list, UINT32_MAX);
    /* B can be entered with arbitrary XMM0; A establishes zero locally. */
    g_assert_cmphex(bi[0].shbr_restore_zero64_exit[TU_TB_INDEX_TARGET], ==, 0);
    g_assert_cmphex(ai[3].shbr_restore_zero64_exit[TU_TB_INDEX_TARGET], ==,
                    self_loop ? 1 : 0);
    g_assert_cmpint(LATXS_SHBR_ON_64(&ai[1]), ==, self_loop);
    g_assert_cmpint(LATXS_SHBR_ON_64(&ai[2]), ==, self_loop);
    checks++;
    for (int i = 0; i < ARRAY_SIZE(ai); i++) {
        g_free(ai[i].info);
    }
    g_free(bi[0].info);
    g_free(a.s_data);
    g_free(b.s_data);
}

static void check_no_zero_run(int cpl, int level)
{
    const uint8_t mul[] = { 0xf2, 0x0f, 0x59, 0xc1 };
    const uint8_t move[] = { 0x66, 0x0f, 0x28, 0xd0 };
    IR1_INST ir1[] = { decode(mul, sizeof(mul)),
                       decode(move, sizeof(move)) };
    TranslationBlock tb = { 0 };
    TranslationBlock *list[] = { &tb };

    tb.s_data = new_tb_data();
    tb.s_data->ir1 = ir1;
    tb.icount = ARRAY_SIZE(ir1);
    tb.flags = HF_OSFXSR_MASK | (cpl << HF_CPL_SHIFT);
    option_shbr = level;
    /* Existing liveness decisions must survive the skipped local pass. */
    ir1[0].shbr_flags = LATXS_SHBR_HIGH64;
    g_assert_false(shbr_tu_has_zero_def64(1, list, false));
    shbr_analyze_known_zero_tu(1, list, UINT32_MAX, false);
    for (int i = 0; i < ARRAY_SIZE(ir1); i++) {
        g_assert_cmphex(ir1[i].shbr_flags, ==,
                        i == 0 ? LATXS_SHBR_HIGH64 : 0);
        g_assert_cmphex(ir1[i].shbr_restore_zero64_before, ==, 0);
        g_assert_cmphex(ir1[i].shbr_restore_zero64_after, ==, 0);
        g_assert_cmphex(ir1[i].shbr_restore_zero64_exit[0], ==, 0);
        g_assert_cmphex(ir1[i].shbr_restore_zero64_exit[1], ==, 0);
        g_free(ir1[i].info);
    }
    checks++;
    g_free(tb.s_data);
}

static void check_eob_indices(void)
{
    TranslationBlock a = { 0 }, b = { 0 };
    TranslationBlock *list[] = { &a, &b };

    a.s_data = new_tb_data();
    b.s_data = new_tb_data();
    a.pc = 0x400000;
    b.pc = 0x400020;
    a.s_data->last_ir1_type = IR1_TYPE_NORMAL;
    a.s_data->next_tb[TU_TB_INDEX_NEXT] = &b;
    option_shbr = 1;
    option_int_opt = IRQ_CHECK_BLOCK_END;
    g_assert_cmpint(shbr_safe_successor_index(2, list, &a,
                    TU_TB_INDEX_NEXT, false, false), ==, -1);
    a.s_data->tb_too_large_ir1_index = 0;
    g_assert_cmpint(shbr_safe_successor_index(2, list, &a,
                    TU_TB_INDEX_NEXT, false, false), ==, 1);
    a.s_data->sys_eob_ir1_index = 0;
    g_assert_cmpint(shbr_safe_successor_index(2, list, &a,
                    TU_TB_INDEX_NEXT, false, false), ==, -1);
    checks += 3;
    g_free(a.s_data);
    g_free(b.s_data);
}

static void check_cross_lane_shift_liveness(void)
{
    const uint8_t movss[] = { 0xf3, 0x0f, 0x10, 0x07 };
    const uint8_t pslldq4[] = { 0x66, 0x0f, 0x73, 0xf8, 0x04 };
    const uint8_t movd[] = { 0x66, 0x0f, 0x7e, 0xc0 };
    const uint8_t movsd[] = { 0xf2, 0x0f, 0x10, 0x07 };
    const uint8_t pslldq8[] = { 0x66, 0x0f, 0x73, 0xf8, 0x08 };
    const uint8_t movq[] = { 0x66, 0x48, 0x0f, 0x7e, 0xc0 };
    IR1_INST high32[] = { decode(movss, sizeof(movss)),
                          decode(pslldq4, sizeof(pslldq4)),
                          decode(movd, sizeof(movd)) };
    IR1_INST high64[] = { decode(movsd, sizeof(movsd)),
                          decode(pslldq8, sizeof(pslldq8)),
                          decode(movq, sizeof(movq)) };
    TranslationBlock tb32 = { 0 };
    TranslationBlock tb64 = { 0 };

    tb32.s_data = new_tb_data();
    tb32.s_data->ir1 = high32;
    tb32.icount = ARRAY_SIZE(high32);
    tb32.flags = HF_OSFXSR_MASK;
    option_shbr = 1;
    shbr_scan_block(&tb32, 0, 32, true, UINT32_MAX, false, false);
    g_assert_false(LATXS_SHBR_ON_32(&high32[0]));

    tb64.s_data = new_tb_data();
    tb64.s_data->ir1 = high64;
    tb64.icount = ARRAY_SIZE(high64);
    tb64.flags = HF_OSFXSR_MASK;
    shbr_scan_block(&tb64, 0, 64, true, UINT32_MAX, false, false);
    g_assert_false(LATXS_SHBR_ON_64(&high64[0]));

    for (int i = 0; i < ARRAY_SIZE(high32); i++) {
        g_free(high32[i].info);
    }
    for (int i = 0; i < ARRAY_SIZE(high64); i++) {
        g_free(high64[i].info);
    }
    g_free(tb32.s_data);
    g_free(tb64.s_data);
    checks += 2;
}

int main(void)
{
    g_assert_cmpint(cs_open(CS_ARCH_X86, CS_MODE_64, &decoder), ==, CS_ERR_OK);
    cs_option(decoder, CS_OPT_DETAIL, CS_OPT_ON);
    check_integer_rules();
    check_eob_indices();
    check_cross_lane_shift_liveness();
    check_integer_liveness();
    for (int level = 1; level <= 3; level++) {
        for (int cpl = 0; cpl <= 3; cpl++) {
            check_no_zero_run(cpl, level);
            check_run(false, false, level, cpl);
            check_run(true, false, level, cpl);
            check_run(true, true, level, cpl);
        }
        check_implicit_zero_clobber(false, level);
        check_implicit_zero_clobber(true, level);
        if (level <= 2) {
            check_recovery_dispatcher_entry(false, level);
            check_recovery_dispatcher_entry(true, level);
        }
    }
    cs_close(&decoder);
    printf("SHBR regression checks: %u passed\n", checks);
    return 0;
}
