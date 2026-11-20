/* SPDX-License-Identifier: GPL-2.0-only */
#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "ir1.h"
#include "latx-options.h"
#include "latx-ghbr.h"
#include "lsenv.h"

#define GHBR_ALL UINT16_MAX

static void ghbr_note_emitted_elision(void);

bool latxs_ghbr_can_elide_write(const IR1_INST *ir1, unsigned int reg)
{
    bool can_elide = latxs_ghbr_enabled() && lsenv && lsenv->tr_data &&
                     lsenv->tr_data->is_tu_mode &&
                     latxs_ghbr_can_omit(ir1, reg);

    if (can_elide) {
        ghbr_note_emitted_elision();
    }
    return can_elide;
}

typedef struct GhbrFacts {
    uint16_t use;
    uint16_t def;
    uint16_t candidate;
    bool barrier;
} GhbrFacts;

typedef struct GhbrState {
    uint16_t in;
    uint16_t out;
} GhbrState;

typedef struct GhbrStats {
    uint64_t tus;
    uint64_t tbs;
    uint64_t candidates;
    uint64_t analysis_hits; /* Guest register writes marked during analysis. */
    uint64_t emitted_elisions; /* Host clearing operations in completed TUs. */
    uint64_t discarded_elisions;
    uint64_t rollbacks;
    uint64_t rejected_live;
    uint64_t barriers;
    uint64_t rejected_tbs;
} GhbrStats;

static GhbrStats ghbr_stats;
static int ghbr_stats_state;
#define GHBR_STATS_ADD(field, value) \
    qatomic_fetch_add(&ghbr_stats.field, (uint64_t)(value))

/* Translation retries must not publish discarded code-generation counts. */
static __thread uint64_t ghbr_pending_elisions;

uint64_t latxs_ghbr_stats_checkpoint(void)
{
    return ghbr_pending_elisions;
}

void latxs_ghbr_stats_restore(uint64_t checkpoint)
{
    if (ghbr_pending_elisions > checkpoint) {
        GHBR_STATS_ADD(discarded_elisions,
                       ghbr_pending_elisions - checkpoint);
        GHBR_STATS_ADD(rollbacks, 1);
    }
    ghbr_pending_elisions = checkpoint;
}

void latxs_ghbr_stats_commit(void)
{
    if (ghbr_pending_elisions) {
        qatomic_fetch_add(&ghbr_stats.emitted_elisions, ghbr_pending_elisions);
        ghbr_pending_elisions = 0;
    }
}

static void ghbr_stats_dump(void)
{
    fprintf(stderr,
            "GHBR_STATS tus=%" PRIu64 " tbs=%" PRIu64
            " candidates=%" PRIu64 " analysis_hits=%" PRIu64
            " emitted_elisions=%" PRIu64
            " discarded_elisions=%" PRIu64 " rollbacks=%" PRIu64
            " rejected_live=%" PRIu64 " barriers=%" PRIu64
            " rejected_tbs=%" PRIu64 "\n",
            qatomic_read(&ghbr_stats.tus), qatomic_read(&ghbr_stats.tbs),
            qatomic_read(&ghbr_stats.candidates),
            qatomic_read(&ghbr_stats.analysis_hits),
            qatomic_read(&ghbr_stats.emitted_elisions),
            qatomic_read(&ghbr_stats.discarded_elisions),
            qatomic_read(&ghbr_stats.rollbacks),
            qatomic_read(&ghbr_stats.rejected_live),
            qatomic_read(&ghbr_stats.barriers),
            qatomic_read(&ghbr_stats.rejected_tbs));
}

static bool ghbr_stats_enabled(void)
{
    int state = qatomic_read(&ghbr_stats_state);
    if (state == 0) {
        const char *value = getenv("LATXS_GHBR_STATS");
        int enabled = value && (!strcmp(value, "1") ||
                                !g_ascii_strcasecmp(value, "on") ||
                                !g_ascii_strcasecmp(value, "yes"));
        int new_state = enabled ? 2 : 1;
        if (qatomic_cmpxchg(&ghbr_stats_state, 0, new_state) == 0 && enabled) {
            if (atexit(ghbr_stats_dump) != 0) {
                qatomic_set(&ghbr_stats_state, 1);
            }
        }
        state = qatomic_read(&ghbr_stats_state);
    }
    return state == 2;
}

static void ghbr_note_emitted_elision(void)
{
    if (ghbr_stats_enabled()) {
        ghbr_pending_elisions++;
    }
}

static bool ghbr_add_address_uses(IR1_OPND *op, GhbrFacts *facts,
                                  bool high_half_live)
{
    x86_reg regs[2] = { op->mem.base, op->mem.index };

    for (unsigned int i = 0; i < ARRAY_SIZE(regs); i++) {
        int reg;

        if (regs[i] == X86_REG_INVALID || regs[i] == X86_REG_RIP ||
            regs[i] == X86_REG_EIP) {
            continue;
        }
        reg = i ? ir1_opnd_index_reg_num(op) : ir1_opnd_base_reg_num(op);
        if (reg < 0 || reg >= 16) {
            return false;
        }
        if (high_half_live) {
            facts->use |= 1U << reg;
        }
    }
    return true;
}

/* Unlisted instructions can fault, call helpers, or have implicit operands. */
static GhbrFacts ghbr_facts(IR1_INST *ir1)
{
    GhbrFacts barrier = { .use = GHBR_ALL, .barrier = true };
    GhbrFacts facts = { 0 };
    bool writes = true, reads_dest = true, conditional = false;
    int count = ir1_opnd_num(ir1);

    if (ir1_prefix(ir1) || ir1->instptn.opc) {
        return barrier;
    }
    switch (ir1_opcode(ir1)) {
    case X86_INS_MOV:
    case X86_INS_MOVABS:
    case X86_INS_MOVZX:
    case X86_INS_MOVSX:
    case X86_INS_MOVSXD:
        reads_dest = false;
        break;
    case X86_INS_LEA:
        reads_dest = false;
        break;
    case X86_INS_ADD:
    case X86_INS_ADC:
    case X86_INS_SUB:
    case X86_INS_SBB:
    case X86_INS_INC:
    case X86_INS_DEC:
    case X86_INS_NEG:
    case X86_INS_NOT:
    case X86_INS_AND:
    case X86_INS_OR:
    case X86_INS_XOR:
    case X86_INS_BSWAP:
    case X86_INS_SHL:
    case X86_INS_SAL:
    case X86_INS_SHR:
    case X86_INS_SAR:
    case X86_INS_ROL:
    case X86_INS_ROR:
    case X86_INS_RCL:
    case X86_INS_RCR:
        break;
    case X86_INS_CMP:
    case X86_INS_TEST:
        writes = false;
        break;
    case X86_INS_CMOVA: case X86_INS_CMOVAE:
    case X86_INS_CMOVB: case X86_INS_CMOVBE:
    case X86_INS_CMOVE: case X86_INS_CMOVNE:
    case X86_INS_CMOVG: case X86_INS_CMOVGE:
    case X86_INS_CMOVL: case X86_INS_CMOVLE:
    case X86_INS_CMOVO: case X86_INS_CMOVNO:
    case X86_INS_CMOVP: case X86_INS_CMOVNP:
    case X86_INS_CMOVS: case X86_INS_CMOVNS:
        /* Only 64-bit CMOV preserves the old high half when not taken. */
        conditional = true;
        break;
    case X86_INS_JMP:
    case X86_INS_JA: case X86_INS_JAE:
    case X86_INS_JB: case X86_INS_JBE:
    case X86_INS_JE: case X86_INS_JNE:
    case X86_INS_JG: case X86_INS_JGE:
    case X86_INS_JL: case X86_INS_JLE:
    case X86_INS_JO: case X86_INS_JNO:
    case X86_INS_JP: case X86_INS_JNP:
    case X86_INS_JS: case X86_INS_JNS:
        return count == 1 && ir1_opnd_is_imm(ir1_get_opnd(ir1, 0)) ?
               facts : barrier;
    case X86_INS_NOP:
        return facts;
    default:
        return barrier;
    }

    for (int i = 0; i < count; i++) {
        IR1_OPND *op = ir1_get_opnd(ir1, i);
        uint16_t mask;
        int reg, width;

        if (ir1_opnd_is_imm(op)) {
            continue;
        }
        if (ir1_opcode(ir1) == X86_INS_LEA && ir1_opnd_is_mem(op)) {
            bool high_half_live = latxs_ir1_addr_size(ir1) == 8 &&
                                  ir1_opnd_size(ir1_get_opnd(ir1, 0)) == 64;

            if (!ghbr_add_address_uses(op, &facts, high_half_live)) {
                return barrier;
            }
            continue;
        }
        if (ir1_opnd_is_mem(op)) {
            if (!latxs_ghbr_ignore_memory()) {
                return barrier;
            }
            if (!ghbr_add_address_uses(op, &facts,
                                       latxs_ir1_addr_size(ir1) == 8)) {
                return barrier;
            }
            continue;
        }
        /* Includes segment/control/debug registers. */
        if (!ir1_opnd_is_gpr(op)) {
            return barrier;
        }
        reg = ir1_opnd_base_reg_num(op);
        if (reg < 0 || reg >= 16) {
            return barrier;
        }
        mask = 1U << reg;
        width = ir1_opnd_size(op);
        if (width == 64 && (i != 0 || reads_dest)) {
            facts.use |= mask;
        }
        if (i == 0 && writes && width >= 32) {
            if (!conditional || width == 32) {
                facts.def |= mask;
            }
            if (width == 32) {
                facts.candidate |= mask;
            }
        }
    }
    return facts;
}

static bool ghbr_tb_enabled(const TranslationBlock *tb)
{
#if defined(CONFIG_LATX_SMC_INST_SELF_CHECK) || defined(CONFIG_LATX_PROFILER)
    return false;
#else
    return (tb->flags & HF_CS64_MASK) &&
           ((tb->flags & HF_CPL_MASK) >> HF_CPL_SHIFT) == 3 &&
           !(tb->flags & (HF_TF_MASK | HF_INHIBIT_IRQ_MASK)) &&
           !(tb_cflags(tb) & (CF_SINGLE_STEP | CF_USE_ICOUNT | CF_LAST_IO));
#endif
}

static uint16_t ghbr_scan(TranslationBlock *tb, uint16_t live, bool mark,
                          bool collect_stats)
{
    IR1_INST *instructions = tb->s_data->ir1;
    if (!ghbr_tb_enabled(tb)) {
        if (collect_stats) {
            GHBR_STATS_ADD(rejected_tbs, 1);
        }
        return GHBR_ALL;
    }
    for (int i = tb->icount; i-- > 0;) {
        IR1_INST *ir1 = &instructions[i];
        GhbrFacts facts = ghbr_facts(ir1);

        if (collect_stats && facts.barrier) {
            GHBR_STATS_ADD(barriers, 1);
        }

        if (mark) {
            ir1->ghbr_opt = facts.candidate & ~live;
            if (collect_stats) {
                uint64_t candidates = __builtin_popcount(facts.candidate);
                uint64_t hits = __builtin_popcount(ir1->ghbr_opt);
                GHBR_STATS_ADD(candidates, candidates);
                GHBR_STATS_ADD(analysis_hits, hits);
                GHBR_STATS_ADD(rejected_live, candidates - hits);
            }
        }
        live = facts.use | (live & ~facts.def);
    }
    return live;
}

/* Only uninterrupted TU edges can carry a noncanonical guest high half. */
static int ghbr_successor(uint32_t count, TranslationBlock **list,
                          unsigned int source, int slot)
{
    TranslationBlock *tb = list[source];
    TranslationBlock *next = tb->s_data->next_tb[slot];

    if (!next || option_int_opt != IRQ_CHECK_BLOCK_END ||
        !ghbr_tb_enabled(tb) || !ghbr_tb_enabled(next) ||
        tb->s_data->sys_eob_ir1_index >= 0 ||
        (tb_cflags(tb) & (CF_NO_GOTO_TB | CF_NO_GOTO_PTR)) ||
        next->pc <= tb->pc ||
        (!tucp_fast_tu_direct() &&
         (next->pc & TARGET_PAGE_MASK) != (tb->pc & TARGET_PAGE_MASK))) {
        return -1;
    }
    for (unsigned int i = 0; i < count; i++) {
        if (list[i] != next) {
            continue;
        }
        if (tb->s_data->last_ir1_type == IR1_TYPE_NORMAL &&
            (tb->s_data->tb_too_large_ir1_index < 0 || i != source + 1)) {
            return -1;
        }
        return i;
    }
    return -1;
}

static uint16_t ghbr_live_out(uint32_t count, TranslationBlock **list,
                              GhbrState *state, unsigned int source)
{
    int slots[2], n;
    uint16_t live = 0;

    switch (list[source]->s_data->last_ir1_type) {
    case IR1_TYPE_BRANCH:
        slots[0] = TU_TB_INDEX_NEXT;
        slots[1] = TU_TB_INDEX_TARGET;
        n = 2;
        break;
    case IR1_TYPE_JUMP:
        slots[0] = TU_TB_INDEX_TARGET;
        n = 1;
        break;
    case IR1_TYPE_NORMAL:
        slots[0] = TU_TB_INDEX_NEXT;
        n = 1;
        break;
    default:
        return GHBR_ALL;
    }
    for (int i = 0; i < n; i++) {
        int next = ghbr_successor(count, list, source, slots[i]);
        live |= next < 0 ? GHBR_ALL : state[next].in;
    }
    return live;
}

void latxs_ghbr_analyze_tu(uint32_t count, TranslationBlock **list)
{
    GhbrState *state;
    bool changed;
    bool collect_stats;

    /* IR1 arrays may be reused by TU retries and AOT retranslation. */
    for (unsigned int i = 0; i < count; i++) {
        IR1_INST *instructions = list[i]->s_data->ir1;
        for (int j = 0; j < list[i]->icount; j++) {
            instructions[j].ghbr_opt = 0;
        }
    }
    if (!latxs_ghbr_enabled() || !count) {
        return;
    }
    collect_stats = ghbr_stats_enabled();
    if (collect_stats) {
        GHBR_STATS_ADD(tus, 1);
        GHBR_STATS_ADD(tbs, count);
    }
    state = g_new0(GhbrState, count);
    do {
        changed = false;
        for (unsigned int i = count; i-- > 0;) {
            uint16_t out = ghbr_live_out(count, list, state, i);
            uint16_t in = ghbr_scan(list[i], out, false, false);

            if (in != state[i].in || out != state[i].out) {
                state[i].in = in;
                state[i].out = out;
                changed = true;
            }
        }
    } while (changed);
    for (unsigned int i = 0; i < count; i++) {
        ghbr_scan(list[i], state[i].out, true, collect_stats);
    }
    g_free(state);
}
