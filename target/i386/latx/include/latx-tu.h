/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __TU_H_
#define __TU_H_
#include "exec/translation-block.h"
#include "latx-options.h"
//#if defined(CONFIG_LATX_AOT) || defined(CONFIG_LATX_TU)
//#include <gmodule.h>
//#include "reg-map.h"
//#include "ir1.h"
//
void get_last_info(TranslationBlock *tb, IR1_INST *pir1);
bool tu_parse_pending_use(TranslationBlock *tb);
//#endif
#ifdef CONFIG_LATX_TU
/*
 * The reason why MAX_TB_IN_CACHE > MAX_TB_IN_TU is
 * TBs in TU sometimes need to translate next guest_pc
 * for call-return optimazation.
 * Although the current implementation do not need them.
 */
#define MAX_TB_IN_TU MIN(option_tu_max_tb, MAX_TB_IN_TS)
#define MAX_TB_IN_AOT_TU MIN(option_aot_tu_max_tb, MAX_TB_IN_TS)
#define MAX_TB_IN_CACHE (MAX_TB_IN_TS + 10)
#define MAX_TB_SIZE 5120
#define MAX_PAGE_IN_TU  TRANSLATION_UNIT_MAX_PAGES
#define MAX_IR1_IN_TU (TARGET_PAGE_SIZE * 2)
/* Max TU gen code size. */
#define MAX_TU_SIZE (MAX_IR1_IN_TU * 16)

#define BCC_OPCODE    0xFC000000
#define BFCC_OPCODE   0xFC000100

#define BCEQZ_OPCODE  0x48000000
#define BCNEZ_OPCODE  0x48000100
#define BEQZ_OPCODE   0x40000000
#define BNEZ_OPCODE   0x44000000

#define BEQ_OPCODE    0x58000000
#define BNE_OPCODE    0x5C000000
#define BLT_OPCODE    0x60000000
#define BGE_OPCODE    0x64000000
#define BLTU_OPCODE   0x68000000
#define BGEU_OPCODE   0x6C000000

#define B_OPCODE      0x50000000
#define BL_OPCODE     0x54000000

#define OFF16_MASK    0xFC0003FF
#define BITS16_MASK   0xFFFF
#define OFF16_SHIFT   10
#define OFF16_MAX     32767
#define OFF16_MIN     -32768
#define BCC_OFF16_MAX (1 << 17) - 1 - 256 * 4
#define BCC_OFF16_MIN -(1 << 17) + 256 * 4

#define OFF20_MASK     0xFC0003E0
#define OFF20_HI_BITS  0x1f
#define OFF20_HI_SHIFT 16

#define OFF20_MAX     0x7ffff
#define OFF20_MIN     -0x80000

#define OFF26_MAX     0x1ffffff
#define OFF26_MIN     -0x2000000

#define MAX_OFFS      0x00020000

/* TUControl manages the translation of TU */
typedef struct TUControl {
    /* the numbers of TB in current TU */
    uint32_t tb_num;
    /* all ir1 number in current TU, which is used for calculate ir1 offset */
    uint32_t ir1_num_in_tu;
    /* all TB pointer in current TU  */
    TranslationBlock *tb_list[MAX_TB_IN_CACHE];
    /* Search GPC to TB in current TU  */
    GTree *tree;
    /* The pages that are being translated in TU */
    struct {
        uint64_t vp;
        uint64_t pp;
        int tb_num;
    } pages[MAX_PAGE_IN_TU];
    int pages_num;
    /* Unique physical pages locked while exploring the current TU. */
    uint64_t locked_pages[MAX_PAGE_IN_TU];
    int locked_pages_num;
    /* Per-tu_gen_code() lock-order retry and fallback state. */
    int lock_order_retry_count;
    struct {
        uint64_t vp;
        uint64_t pp;
    } retry_pages[MAX_PAGE_IN_TU];
    int retry_pages_num;
    uint64_t pending_protect_pages[MAX_PAGE_IN_TU];
    int pending_protect_num;
    int protect_retry_count;
    bool disable_cross_page_for_run;
    /* force to do tb flush */
    int force_tb_flush;
    struct {
        int ef_valid; /* 0 = invalid, 1 = next */
        int ef_use_imm;
        IR2_OPCODE ef_opcode;
        IR2_OPND ef_opnd0;
        IR2_OPND ef_opnd1;
        int ef_imm;
    } eflag_opt_data;
} TUControl;
#define TU_EFLAG_OPT_VALID(tu)  ((tu)->eflag_opt_data.ef_valid)
#define TU_EFLAG_OPT_RESET(tu)  ((tu)->eflag_opt_data.ef_valid = 0)
#define TU_EFLAG_OPT_SET(tu) do {                     \
    (tu)->eflag_opt_data.ef_valid = 1;                \
    (tu)->eflag_opt_data.ef_use_imm = 0;              \
} while (0)
#define TU_EFLAG_OPT_SET_I(tu, opc, opnd0, imm) do {  \
    (tu)->eflag_opt_data.ef_valid = 1;                \
    (tu)->eflag_opt_data.ef_use_imm = 1;              \
    (tu)->eflag_opt_data.ef_opcode = opc;             \
    (tu)->eflag_opt_data.ef_opnd0 = opnd0;            \
    (tu)->eflag_opt_data.ef_imm = imm;                \
} while (0)
#define TU_EFLAG_OPT_GEN(tu) do {                   \
    if (TU_EFLAG_OPT_VALID(tu)) {                   \
        if ((tu)->eflag_opt_data.ef_use_imm) {      \
            la_out1i((tu)->eflag_opt_data.ef_opcode, \
              &((tu)->eflag_opt_data.ef_opnd0),     \
              (tu)->eflag_opt_data.ef_imm);         \
        } else {                                    \
            la_out2((tu)->eflag_opt_data.ef_opcode, \
              &((tu)->eflag_opt_data.ef_opnd0),     \
              &((tu)->eflag_opt_data.ef_opnd1));    \
        }                                           \
        TU_EFLAG_OPT_RESET(tu);                     \
    }                                               \
} while (0)
//
typedef enum TU_TB_START_TYPE {
    TU_TB_START_NONE = 0,
    TU_TB_START_JMP,
    TU_TB_START_NORMAL,
    TU_TB_START_ENTRY
} TU_TB_START_TYPE;

#define TU_EXPLORE_OK                  0
#define TU_EXPLORE_EXISTED              1
#define TU_EXPLORE_RESTART_LOCK_ORDER  2
#define TU_EXPLORE_FALLBACK             3
#define TU_EXPLORE_NEED_PROTECT         4

#define TU_TRANSLATE_OK                0
#define TU_TRANSLATE_RETRY            -1
#define TU_TRANSLATE_HRAS_LONG_CHECK  -2
#define TU_TRANSLATE_FALLBACK         -3

#define TU_ASSEMBLE_ERR_BUFFER_OVERFLOW  -1
#define TU_ASSEMBLE_ERR_TB_INFO          -2
#define TU_ASSEMBLE_RETRY_HRAS_LONG_CHECK -3
#define TU_ASSEMBLE_ERR_HRAS_NJC_RANGE    -4

extern __thread TUControl *tu_data;

static inline int tu_need_tb_flush(void) {
    return tu_data->force_tb_flush;
}
static inline void tu_force_tb_flush(void) {
    tu_data->force_tb_flush = 1;
}
static inline void tu_reset_tb_flush(void) {
    tu_data->force_tb_flush = 0;
}

void tu_enough_space(CPUState *cpu);
void tu_trees_reset(void);
TranslationBlock *tu_tree_lookup(target_ulong pc);
void tu_control_init(void);
TranslationBlock* tb_create(CPUState *cpu, target_ulong pc,
        target_ulong cs_base, uint32_t flags, int cflags,
        int max_insns, bool is_first_tb, TU_TB_START_TYPE mode,
        const TranslationBlock *hras_caller, int *result);
void tu_push_back(TranslationBlock *tb);
TranslationBlock *tu_gen_code(CPUState *cpu, target_ulong pc,
                              target_ulong cs_base, uint32_t flags,
                              int cflags);
int translate_tu(CPUState *cpu, uint32 tb_num_in_tu,
                 TranslationBlock **tb_list);
TranslationUnitPages *tb_build_pages_desc(TCGContext *tcg_ctx,
                                           const TranslationBlock *tb);
bool tu_pages_valid(CPUState *cpu, const TranslationBlock *tb);
uint32_t tu_pages_alloc_id(void);
void tu_generation_invalidate(CPUState *cpu);
void tu_generation_reset_ids(void);
static inline bool tu_jmp_cache_can_insert(CPUState *cpu,
                                           const TranslationBlock *tb)
{
    /* A descriptor is a validity contract, independent of hash/option bits. */
    return !TB_TU_PAGES(tb) || tu_pages_valid(cpu, tb);
}
void solve_tb_overlap(uint32 tb_num_in_tu,
		TranslationBlock **tb_list, int max_insns);

int ir1_is_eob_in_tu(IR1_INST *pir1);

int translate_tb_in_tu(uint32 tb_num_in_tu, TranslationBlock **tb_list);
void tu_reset_tb(TranslationBlock *tb);
int tu_relocat_target_branch(TranslationBlock * tb);
void tu_relocat_next_branch(TranslationBlock * tb);
void bcc_ins_recover(TranslationBlock *tb);
IR2_OPCODE bcc_opcode_convert(IR2_OPCODE bcc_op);
uint bcc_ins_convert(uint convert_insn);
//#ifdef CONFIG_LATX_DEBUG
//void print_ir1(TranslationBlock* tb);
//void print_tu_tb(TranslationBlock *tb);
#endif
//
///*typedef struct PageDesc { */
///*    /1* list of TBs intersecting this ram page *1/ */
///*    uintptr_t first_tb; */
///*#ifdef CONFIG_SOFTMMU */
///*    /1* */
///*     *in order to optimize self modifying code, we count the number */
///*     *of lookups we do to a given page to use a bitmap */
///*     *1/ */
///*    unsigned long *code_bitmap; */
///*    unsigned int code_write_count; */
///*#else */
///*    unsigned long flags; */
///*    void *target_data; */
///*#endif */
///*#ifndef CONFIG_USER_ONLY */
///*    QemuSpin lock; */
///*#endif */
///*#ifdef CONFIG_LATX_AOT2 */
///*    uint8_t page_state; */
///*#endif */
///*} PageDesc; */
///*PageDesc *page_find_alloc(tb_page_addr_t index, int alloc); */
//
//bool judge_tu_eflag_gen(void *tb_in_tu);
/* JIT exploration sorts unique guest PCs before analysis and code generation.
 * A canonical TB can share a PC with a private copy, so also check identity. */
static inline int tu_sorted_tb_index(uint32_t count, TranslationBlock **list,
                                     TranslationBlock *tb)
{
    uint32_t lo = 0, hi = count;

    if (!tb) {
        return -1;
    }
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        if (list[mid]->pc < tb->pc) {
            lo = mid + 1;
        } else if (list[mid]->pc > tb->pc) {
            hi = mid;
        } else {
            return list[mid] == tb ? mid : -1;
        }
    }
    return -1;
}

bool tb_is_in_tb_list(TranslationBlock *target_tb);
bool tb_is_adjacent(TranslationBlock *front_tb, TranslationBlock *back_tb);
void tu_jcc_nop_gen(TranslationBlock *tb, int opnd_num, bool is_target);
void add_tu(IR2_OPCODE jcc_op, IR2_OPND cond0, IR2_OPND cond1, IR2_OPND target_label_opnd, IR1_INST *pir1);
TranslationBlock **retranslate_by_tu(CPUState *cpu, target_ulong page_addr, uintptr_t hva,
                            TranslationBlock **page_tb_list, int *page_tb_num);
typedef enum AOTCrossPageTUResult {
    AOT_TU_SAVE_OK,
    /* No record was written; rebuild a smaller TU from original templates. */
    AOT_TU_SAVE_RETRY,
    AOT_TU_SAVE_ERROR,
} AOTCrossPageTUResult;
/* The visitor releases scratch code/auxiliary storage on every return. */
typedef AOTCrossPageTUResult (*AOTCrossPageTUVisitor)(
        TranslationBlock **tb_list, int tb_num, void *opaque);
bool retranslate_by_tu_cross_page(CPUState *cpu,
                            TranslationBlock **tb_templates, int tb_num,
                            AOTCrossPageTUVisitor visitor, void *opaque);
#ifdef CONFIG_LATX_PROFILER
void add_tu_profiler(TranslationBlock *tb);
#endif
//#endif
void tu_tbmini_set_pointer(uint64_t* tbm, uint64_t tb_addr,  uint64_t tc_size);
TranslationBlock *gen_check_tu(CPUState *cpu, TranslationBlock **aot_tb_list, int tb_count);

#define GEN_TU_FUN_AND_RETURN2(_opcode, _cond0, _cond1, _target_label, _pir1) do { \
    TRANSLATION_DATA *td = lsenv->tr_data; \
    TranslationBlock *tb = td->curr_tb; \
    if (td->is_tu_mode && (tb->s_data->next_tb[TU_TB_INDEX_NEXT] || tb->s_data->next_tb[TU_TB_INDEX_TARGET])) { \
        add_tu(_opcode, _cond0, _cond1, _target_label, _pir1); \
        return true; \
    } \
} while (0)

#define GEN_TU_FUN_AND_RETURN(_opcode, _cond0, _target_label, _pir1) do { \
    IR2_OPND _cond1 = {._type = IR2_OPND_INV}; \
    GEN_TU_FUN_AND_RETURN2(_opcode, _cond0, _cond1, _target_label, _pir1); \
} while (0)

#endif
