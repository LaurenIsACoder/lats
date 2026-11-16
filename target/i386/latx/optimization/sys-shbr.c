/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * XMM high-bit liveness follows the architectural operand semantics in the
 * Intel 64 and IA-32 instruction reference.  The generated opcode table is
 * the instruction-semantics source; decoder access flags identify operand
 * roles inside those audited rules and are never used for unlisted opcodes.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "ir1.h"
#include "latx-options.h"
#include "latx-shbr.h"
#include "sys-aot.h"
#include "latx-tu.h"

#ifdef CONFIG_LATX_TU

#if defined(CONFIG_CAPSTONE_DIET) && \
    !defined(CONFIG_LATX_CAPSTONE_OP_ACCESS)
#error "SHBR requires decoded x86 operand access metadata"
#endif

#define SHBR_XMM_ALL UINT16_MAX
#define WRAP(ins) X86_INS_##ins

typedef enum ShbrRule {
    SHBR_RULE_NONE,
    SHBR_RULE_BARRIER,
    SHBR_RULE_FULL_MOVE,
    SHBR_RULE_RMW,
    SHBR_RULE_RMW_READ,
    SHBR_RULE_RMW_BARRIER,
    SHBR_RULE_READ,
    SHBR_RULE_MOVSS,
    SHBR_RULE_MOVSD,
    SHBR_RULE_MOVD,
    SHBR_RULE_MOVQ,
    SHBR_RULE_SCALAR32,
    SHBR_RULE_SCALAR64,
    SHBR_RULE_CVTSD2SS,
    SHBR_RULE_CVTSS2SD,
    SHBR_RULE_ACCESS,
    SHBR_RULE_ACCESS_READ,
    SHBR_RULE_DEST,
    SHBR_RULE_READ_ACCESS,
    SHBR_RULE_OTHER,
    SHBR_RULE_IGNORE,
    SHBR_RULE_BROADCAST,
    SHBR_RULE_EXTRACT,
    SHBR_RULE_IMPLICIT,
} ShbrRule;

typedef struct ShbrExplicitSemantic {
    uint8_t high32;
    uint8_t high64;
} ShbrExplicitSemantic;

static const ShbrExplicitSemantic
shbr_explicit_semantics[X86_INS_ENDING] = {
#include "sys-shbr-semantics.inc"
};

typedef struct ShbrFacts {
    uint16_t read;
    uint16_t def;
    uint16_t dep;
    uint16_t candidate;
    bool memory_barrier;
} ShbrFacts;

typedef struct ShbrTbState {
    uint16_t live_in;
    uint16_t live_out;
} ShbrTbState;

/* Per-analysis results: normal/recovery mode, then next/target edge.
 * Zero means unqueried; store index + 2 so a rejected edge (-1) is cached. */
typedef int ShbrSuccessors[2][2];

typedef struct ShbrStats {
    uint64_t tus;
    uint64_t tbs;
    uint64_t ir1s;
    uint64_t memory_barriers;
    uint64_t candidates32;
    uint64_t candidates64;
    uint64_t hits32;
    uint64_t hits64;
    uint64_t rejected_live32;
    uint64_t rejected_live64;
    uint64_t high32_tus;
    uint64_t high32_skipped_tus;
    uint64_t known_zero_candidates64;
    uint64_t known_zero_new_hits64;
    uint64_t known_zero_restores64;
    uint64_t known_zero_cross_tb_hits64;
    uint64_t known_zero_cold_restores64;
    uint64_t known_zero_recover_edges;
    uint64_t recover_tentative64;
    uint64_t recover_local_zero64;
    uint64_t recover_invalid_tus;
    uint64_t edges_safe;
    uint64_t edges_aggressive;
    uint64_t edges_outside_tu;
    uint64_t edges_interrupt_mode;
    uint64_t edges_tb_state;
    uint64_t edges_backward;
    uint64_t edges_cross_page;
    uint64_t edges_normal_eob;
} ShbrStats;

static ShbrStats shbr_stats;
/* 0: uninitialized, 1: disabled, 2: enabled. */
static int shbr_stats_state;

static void shbr_stats_dump(void)
{
    fprintf(stderr,
            "SHBR_STATS tus=%" PRIu64 " tbs=%" PRIu64 " ir1=%" PRIu64
            " memory_barriers=%" PRIu64
            " candidates32=%" PRIu64 " hits32=%" PRIu64
            " rejected_live32=%" PRIu64 " candidates64=%" PRIu64
            " hits64=%" PRIu64 " rejected_live64=%" PRIu64
            " high32_tus=%" PRIu64 " high32_skipped_tus=%" PRIu64
            " known_zero_candidates64=%" PRIu64
            " known_zero_new_hits64=%" PRIu64
            " known_zero_restores64=%" PRIu64
            " known_zero_cross_tb_hits64=%" PRIu64
            " known_zero_cold_restores64=%" PRIu64
            " known_zero_recover_edges=%" PRIu64
            " recover_tentative64=%" PRIu64
            " recover_local_zero64=%" PRIu64
            " recover_invalid_tus=%" PRIu64 "\n",
            qatomic_read(&shbr_stats.tus),
            qatomic_read(&shbr_stats.tbs),
            qatomic_read(&shbr_stats.ir1s),
            qatomic_read(&shbr_stats.memory_barriers),
            qatomic_read(&shbr_stats.candidates32),
            qatomic_read(&shbr_stats.hits32),
            qatomic_read(&shbr_stats.rejected_live32),
            qatomic_read(&shbr_stats.candidates64),
            qatomic_read(&shbr_stats.hits64),
            qatomic_read(&shbr_stats.rejected_live64),
            qatomic_read(&shbr_stats.high32_tus),
            qatomic_read(&shbr_stats.high32_skipped_tus),
            qatomic_read(&shbr_stats.known_zero_candidates64),
            qatomic_read(&shbr_stats.known_zero_new_hits64),
            qatomic_read(&shbr_stats.known_zero_restores64),
            qatomic_read(&shbr_stats.known_zero_cross_tb_hits64),
            qatomic_read(&shbr_stats.known_zero_cold_restores64),
            qatomic_read(&shbr_stats.known_zero_recover_edges),
            qatomic_read(&shbr_stats.recover_tentative64),
            qatomic_read(&shbr_stats.recover_local_zero64),
            qatomic_read(&shbr_stats.recover_invalid_tus));
    fprintf(stderr,
            "SHBR_EDGE_STATS safe=%" PRIu64 " aggressive=%" PRIu64
            " outside_tu=%" PRIu64
            " interrupt_mode=%" PRIu64 " tb_state=%" PRIu64
            " backward=%" PRIu64 " cross_page=%" PRIu64
            " normal_eob=%" PRIu64 "\n",
            qatomic_read(&shbr_stats.edges_safe),
            qatomic_read(&shbr_stats.edges_aggressive),
            qatomic_read(&shbr_stats.edges_outside_tu),
            qatomic_read(&shbr_stats.edges_interrupt_mode),
            qatomic_read(&shbr_stats.edges_tb_state),
            qatomic_read(&shbr_stats.edges_backward),
            qatomic_read(&shbr_stats.edges_cross_page),
            qatomic_read(&shbr_stats.edges_normal_eob));
}

static bool shbr_stats_enabled(void)
{
    int state = qatomic_read(&shbr_stats_state);

    if (unlikely(state == 0)) {
        const char *value = getenv("LATXS_SHBR_STATS");
        int new_state = value && value[0] && strcmp(value, "0") ? 2 : 1;

        state = qatomic_cmpxchg(&shbr_stats_state, 0, new_state);
        if (state == 0) {
            state = new_state;
            if (state == 2 && atexit(shbr_stats_dump) != 0) {
                fprintf(stderr, "WARNING: cannot register SHBR statistics "
                        "dump\n");
            }
        } else {
            state = qatomic_read(&shbr_stats_state);
        }
    }
    return state == 2;
}

#define SHBR_STATS_ADD(field, value) \
    qatomic_fetch_add(&shbr_stats.field, (uint64_t)(value))

static inline bool shbr_is_xmm(const IR1_OPND *opnd)
{
    return ir1_opnd_is_xmm((IR1_OPND *)opnd);
}

static inline bool shbr_is_vector(const IR1_OPND *opnd)
{
    return ir1_opnd_is_xmm((IR1_OPND *)opnd) ||
           ir1_opnd_is_ymm((IR1_OPND *)opnd);
}

static inline uint16_t shbr_xmm_mask(const IR1_OPND *opnd)
{
    int reg = ir1_opnd_base_reg_num((IR1_OPND *)opnd);

    if (reg < 0 || reg >= 16) {
        return 0;
    }
    return 1U << reg;
}

static inline uint16_t shbr_vector_mask(const IR1_OPND *opnd)
{
    int reg = ir1_opnd_base_reg_num((IR1_OPND *)opnd);

    if (reg < 0 || reg >= 16) {
        return 0;
    }
    return 1U << reg;
}

static bool shbr_has_vector_operand(IR1_INST *ir1)
{
    int opnd_num = ir1_get_opnd_num(ir1);

    for (int i = 0; i < opnd_num; i++) {
        if (shbr_is_vector(ir1_get_opnd(ir1, i))) {
            return true;
        }
    }
    return false;
}

static bool shbr_has_vector_index(IR1_OPND *opnd)
{
    x86_reg index;

    if (!ir1_opnd_is_mem(opnd) || !ir1_opnd_has_index(opnd)) {
        return false;
    }
    index = ir1_opnd_index_reg(opnd);
    return (index >= X86_REG_XMM0 && index <= X86_REG_XMM15) ||
           (index >= X86_REG_YMM0 && index <= X86_REG_YMM15);
}

static uint16_t shbr_vector_index_mask(IR1_OPND *opnd)
{
    x86_reg index = ir1_opnd_index_reg(opnd);
    int reg = index >= X86_REG_YMM0 ? index - X86_REG_YMM0 :
                                      index - X86_REG_XMM0;

    return reg >= 0 && reg < 16 ? 1U << reg : 0;
}

static uint16_t shbr_explicit_xmm_mask(IR1_INST *ir1, int first)
{
    uint16_t mask = 0;
    int opnd_num = ir1_get_opnd_num(ir1);

    for (int i = first; i < opnd_num; i++) {
        IR1_OPND *opnd = ir1_get_opnd(ir1, i);

        if (shbr_is_xmm(opnd)) {
            mask |= shbr_xmm_mask(opnd);
        }
    }
    return mask;
}

static bool shbr_has_memory_operand(IR1_INST *ir1)
{
    int opnd_num = ir1_get_opnd_num(ir1);

    for (int i = 0; i < opnd_num; i++) {
        if (ir1_opnd_is_mem(ir1_get_opnd(ir1, i))) {
            return true;
        }
    }
    return false;
}

static bool shbr_has_implicit_memory_access(IR1_INST *ir1)
{
    /* MASKMOVDQU stores through an implicit (E)DI/RDI address. */
    return ir1_opcode(ir1) == X86_INS_MASKMOVDQU;
}

static uint32_t shbr_required_cpuid_ext_feature(IR1_INST *ir1)
{
    switch (ir1_opcode(ir1)) {
    case X86_INS_HADDPD:
    case X86_INS_HADDPS:
    case X86_INS_HSUBPD:
    case X86_INS_HSUBPS:
    case X86_INS_MOVSHDUP:
    case X86_INS_MOVSLDUP:
    case X86_INS_MOVDDUP:
    case X86_INS_ADDSUBPS:
    case X86_INS_ADDSUBPD:
        return CPUID_EXT_SSE3;

    case X86_INS_PALIGNR:
    case X86_INS_PSHUFB:
    case X86_INS_PHADDW:
    case X86_INS_PHADDD:
    case X86_INS_PHADDSW:
    case X86_INS_PMADDUBSW:
    case X86_INS_PHSUBW:
    case X86_INS_PHSUBD:
    case X86_INS_PHSUBSW:
    case X86_INS_PSIGNB:
    case X86_INS_PSIGNW:
    case X86_INS_PSIGND:
    case X86_INS_PMULHRSW:
    case X86_INS_PABSB:
    case X86_INS_PABSW:
    case X86_INS_PABSD:
        return CPUID_EXT_SSSE3;

    case X86_INS_ROUNDPS:
    case X86_INS_ROUNDPD:
    case X86_INS_ROUNDSS:
    case X86_INS_ROUNDSD:
    case X86_INS_BLENDPS:
    case X86_INS_BLENDPD:
    case X86_INS_PBLENDW:
    case X86_INS_DPPS:
    case X86_INS_DPPD:
    case X86_INS_MPSADBW:
    case X86_INS_PBLENDVB:
    case X86_INS_BLENDVPS:
    case X86_INS_BLENDVPD:
    case X86_INS_PTEST:
    case X86_INS_PMOVSXBW:
    case X86_INS_PMOVSXBD:
    case X86_INS_PMOVSXBQ:
    case X86_INS_PMOVSXWD:
    case X86_INS_PMOVSXWQ:
    case X86_INS_PMOVSXDQ:
    case X86_INS_PMULDQ:
    case X86_INS_PCMPEQQ:
    case X86_INS_PACKUSDW:
    case X86_INS_PMOVZXBW:
    case X86_INS_PMOVZXBD:
    case X86_INS_PMOVZXBQ:
    case X86_INS_PMOVZXWD:
    case X86_INS_PMOVZXWQ:
    case X86_INS_PMOVZXDQ:
    case X86_INS_PMINSB:
    case X86_INS_PMINSD:
    case X86_INS_PMINUW:
    case X86_INS_PMINUD:
    case X86_INS_PMAXSB:
    case X86_INS_PMAXSD:
    case X86_INS_PMAXUW:
    case X86_INS_PMAXUD:
    case X86_INS_PMULLD:
    case X86_INS_PHMINPOSUW:
    case X86_INS_PEXTRQ:
    case X86_INS_PEXTRD:
    case X86_INS_PEXTRB:
    case X86_INS_PINSRB:
    case X86_INS_PINSRD:
    case X86_INS_PINSRQ:
    case X86_INS_INSERTPS:
    case X86_INS_EXTRACTPS:
        return CPUID_EXT_SSE41;

    case X86_INS_PCMPESTRM:
    case X86_INS_PCMPESTRI:
    case X86_INS_PCMPISTRM:
    case X86_INS_PCMPISTRI:
    case X86_INS_PCMPGTQ:
        return CPUID_EXT_SSE42;

    case X86_INS_AESKEYGENASSIST:
    case X86_INS_AESIMC:
    case X86_INS_AESENC:
    case X86_INS_AESENCLAST:
    case X86_INS_AESDEC:
    case X86_INS_AESDECLAST:
        return CPUID_EXT_AES;

    case X86_INS_PCLMULQDQ:
        return CPUID_EXT_PCLMULQDQ;

    default:
        return 0;
    }
}

static bool shbr_missing_cpuid_feature(IR1_INST *ir1,
        uint32_t cpuid_ext_features)
{
    uint32_t required = shbr_required_cpuid_ext_feature(ir1);

    return required && !(cpuid_ext_features & required);
}

static bool shbr_is_self_zeroing(IR1_INST *ir1)
{
    switch (ir1_opcode(ir1)) {
    case X86_INS_XORPS:
    case X86_INS_XORPD:
    case X86_INS_PXOR:
    case X86_INS_PSUBB:
    case X86_INS_PSUBW:
    case X86_INS_PSUBD:
    case X86_INS_PSUBQ:
    case X86_INS_PSUBSB:
    case X86_INS_PSUBSW:
    case X86_INS_PSUBUSB:
    case X86_INS_PSUBUSW:
    case X86_INS_ANDNPS:
    case X86_INS_ANDNPD:
    case X86_INS_PANDN:
        return true;
    default:
        return false;
    }
}

/*
 * These rules describe only high-bit data flow.  Rules that can move a high
 * input into an architecturally visible low result, or can raise a packed
 * floating-point exception from a high lane, use RMW_READ/READ.
 * Unlisted instructions are barriers by construction.
 */
static ShbrRule shbr_rule(IR1_INST *ir1)
{
    switch (ir1_opcode(ir1)) {
    case X86_INS_MOVSS:
        return SHBR_RULE_MOVSS;
    case X86_INS_MOVSD:
        return SHBR_RULE_MOVSD;
    case X86_INS_MOVD:
        return SHBR_RULE_MOVD;
    case X86_INS_MOVQ:
        return SHBR_RULE_MOVQ;

    case X86_INS_MOVAPS:
    case X86_INS_MOVAPD:
    case X86_INS_MOVUPS:
    case X86_INS_MOVUPD:
    case X86_INS_MOVDQA:
    case X86_INS_MOVDQU:
    case X86_INS_MOVNTDQ:
    case X86_INS_MOVNTPD:
    case X86_INS_MOVNTPS:
    case X86_INS_MOVNTDQA:
    case X86_INS_LDDQU:
        return SHBR_RULE_FULL_MOVE;

    case X86_INS_PADDB:
    case X86_INS_PADDW:
    case X86_INS_PADDD:
    case X86_INS_PADDQ:
    case X86_INS_PADDSB:
    case X86_INS_PADDSW:
    case X86_INS_PADDUSB:
    case X86_INS_PADDUSW:
    case X86_INS_PSUBB:
    case X86_INS_PSUBW:
    case X86_INS_PSUBD:
    case X86_INS_PSUBQ:
    case X86_INS_PSUBSB:
    case X86_INS_PSUBSW:
    case X86_INS_PSUBUSB:
    case X86_INS_PSUBUSW:
    case X86_INS_PMADDWD:
    case X86_INS_PMADDUBSW:
    case X86_INS_PMULDQ:
    case X86_INS_PMULUDQ:
    case X86_INS_PMULLW:
    case X86_INS_PMULLD:
    case X86_INS_PMULHW:
    case X86_INS_PMULHUW:
    case X86_INS_PMULHRSW:
    case X86_INS_PABSB:
    case X86_INS_PABSW:
    case X86_INS_PABSD:
    case X86_INS_PAVGB:
    case X86_INS_PAVGW:
    case X86_INS_PMINSB:
    case X86_INS_PMINSW:
    case X86_INS_PMINSD:
    case X86_INS_PMINUB:
    case X86_INS_PMINUW:
    case X86_INS_PMINUD:
    case X86_INS_PMAXSB:
    case X86_INS_PMAXSW:
    case X86_INS_PMAXSD:
    case X86_INS_PMAXUB:
    case X86_INS_PMAXUW:
    case X86_INS_PMAXUD:
    case X86_INS_PCMPEQB:
    case X86_INS_PCMPEQW:
    case X86_INS_PCMPEQD:
    case X86_INS_PCMPEQQ:
    case X86_INS_PCMPGTB:
    case X86_INS_PCMPGTW:
    case X86_INS_PCMPGTD:
    case X86_INS_PCMPGTQ:
    case X86_INS_PAND:
    case X86_INS_PANDN:
    case X86_INS_POR:
    case X86_INS_PXOR:
    case X86_INS_ANDPS:
    case X86_INS_ANDNPS:
    case X86_INS_ORPS:
    case X86_INS_XORPS:
    case X86_INS_ANDPD:
    case X86_INS_ANDNPD:
    case X86_INS_ORPD:
    case X86_INS_XORPD:
    case X86_INS_BLENDPS:
    case X86_INS_BLENDPD:
    case X86_INS_PBLENDW:
        return SHBR_RULE_RMW;

    case X86_INS_ADDPS:
    case X86_INS_ADDPD:
    case X86_INS_ADDSUBPS:
    case X86_INS_ADDSUBPD:
    case X86_INS_SUBPS:
    case X86_INS_SUBPD:
    case X86_INS_MULPS:
    case X86_INS_MULPD:
    case X86_INS_DIVPS:
    case X86_INS_DIVPD:
    case X86_INS_MAXPS:
    case X86_INS_MAXPD:
    case X86_INS_MINPS:
    case X86_INS_MINPD:
    case X86_INS_SQRTPS:
    case X86_INS_SQRTPD:
    case X86_INS_HADDPS:
    case X86_INS_HADDPD:
    case X86_INS_HSUBPS:
    case X86_INS_HSUBPD:
    case X86_INS_CMPPS:
    case X86_INS_CMPPD:
    case X86_INS_DPPD:
    case X86_INS_DPPS:
    case X86_INS_AESDEC:
    case X86_INS_AESDECLAST:
    case X86_INS_AESENC:
    case X86_INS_AESENCLAST:
    case X86_INS_AESIMC:
    case X86_INS_AESKEYGENASSIST:
    case X86_INS_PCLMULQDQ:
        return SHBR_RULE_RMW_BARRIER;

    case X86_INS_PACKSSWB:
    case X86_INS_PACKSSDW:
    case X86_INS_PACKUSWB:
    case X86_INS_PACKUSDW:
    case X86_INS_PUNPCKHBW:
    case X86_INS_PUNPCKHWD:
    case X86_INS_PUNPCKHDQ:
    case X86_INS_PUNPCKHQDQ:
    case X86_INS_UNPCKHPS:
    case X86_INS_UNPCKHPD:
    case X86_INS_PSHUFB:
    case X86_INS_PSHUFD:
    case X86_INS_PSHUFHW:
    case X86_INS_PSHUFLW:
    case X86_INS_SHUFPS:
    case X86_INS_SHUFPD:
    case X86_INS_PALIGNR:
    case X86_INS_PSADBW:
    case X86_INS_PHADDD:
    case X86_INS_PHADDSW:
    case X86_INS_PHADDW:
    case X86_INS_PHSUBD:
    case X86_INS_PHSUBSW:
    case X86_INS_PHSUBW:
    case X86_INS_PHMINPOSUW:
    case X86_INS_INSERTPS:
        return SHBR_RULE_RMW_READ;

    case X86_INS_MOVMSKPS:
    case X86_INS_MOVMSKPD:
    case X86_INS_PMOVMSKB:
    case X86_INS_PTEST:
    case X86_INS_PCMPESTRI:
    case X86_INS_PCMPESTRM:
    case X86_INS_PCMPISTRI:
    case X86_INS_PCMPISTRM:
        return SHBR_RULE_BARRIER;

    case X86_INS_EXTRACTPS:
    case X86_INS_PEXTRB:
    case X86_INS_PEXTRW:
    case X86_INS_PEXTRD:
    case X86_INS_PEXTRQ:
        return SHBR_RULE_READ;

    case X86_INS_ADDSS:
    case X86_INS_SUBSS:
    case X86_INS_MULSS:
    case X86_INS_DIVSS:
    case X86_INS_MAXSS:
    case X86_INS_MINSS:
    case X86_INS_SQRTSS:
    case X86_INS_RCPSS:
    case X86_INS_RSQRTSS:
    case X86_INS_CMPSS:
    case X86_INS_CMPEQSS:
    case X86_INS_CMPLTSS:
    case X86_INS_CMPLESS:
    case X86_INS_CMPUNORDSS:
    case X86_INS_CMPNEQSS:
    case X86_INS_CMPNLTSS:
    case X86_INS_CMPNLESS:
    case X86_INS_CMPORDSS:
    case X86_INS_ROUNDSS:
        return SHBR_RULE_SCALAR32;

    case X86_INS_ADDSD:
    case X86_INS_SUBSD:
    case X86_INS_MULSD:
    case X86_INS_DIVSD:
    case X86_INS_MAXSD:
    case X86_INS_MINSD:
    case X86_INS_SQRTSD:
    case X86_INS_CMPSD:
    case X86_INS_CMPEQSD:
    case X86_INS_CMPLTSD:
    case X86_INS_CMPLESD:
    case X86_INS_CMPUNORDSD:
    case X86_INS_CMPNEQSD:
    case X86_INS_CMPNLTSD:
    case X86_INS_CMPNLESD:
    case X86_INS_CMPORDSD:
    case X86_INS_ROUNDSD:
        return SHBR_RULE_SCALAR64;

    case X86_INS_CVTSD2SS:
        return SHBR_RULE_CVTSD2SS;
    case X86_INS_CVTSS2SD:
        return SHBR_RULE_CVTSS2SD;

    default:
        return SHBR_RULE_BARRIER;
    }
}

static void shbr_apply_full_move(IR1_INST *ir1, ShbrFacts *facts)
{
    uint16_t dest = 0;

    if (ir1_get_opnd_num(ir1) > 0 && shbr_is_xmm(ir1_get_opnd(ir1, 0))) {
        dest = shbr_xmm_mask(ir1_get_opnd(ir1, 0));
    }
    if (dest) {
        facts->def |= dest;
        facts->dep |= shbr_explicit_xmm_mask(ir1, 1);
    } else {
        facts->read |= shbr_explicit_xmm_mask(ir1, 0);
    }
}

static void shbr_apply_rmw(IR1_INST *ir1, ShbrFacts *facts,
        bool direct_read)
{
    uint16_t explicit = shbr_explicit_xmm_mask(ir1, 0);
    uint16_t dest = 0;

    if (ir1_get_opnd_num(ir1) > 0 && shbr_is_xmm(ir1_get_opnd(ir1, 0))) {
        dest = shbr_xmm_mask(ir1_get_opnd(ir1, 0));
    }
    if (!dest) {
        facts->read |= explicit;
        return;
    }

    facts->def |= dest;
    facts->dep |= explicit;
    if (direct_read) {
        facts->read |= explicit;
    }

    if (ir1_get_opnd_num(ir1) > 1 &&
        shbr_is_xmm(ir1_get_opnd(ir1, 1)) &&
        shbr_xmm_mask(ir1_get_opnd(ir1, 1)) == dest &&
        shbr_is_self_zeroing(ir1)) {
        facts->dep &= ~dest;
    }
}

static void shbr_apply_movss(IR1_INST *ir1, ShbrFacts *facts,
        int high_bits)
{
    IR1_OPND *dest;
    IR1_OPND *src;
    uint16_t dest_mask;

    if (ir1_get_opnd_num(ir1) < 2) {
        facts->read = SHBR_XMM_ALL;
        return;
    }
    dest = ir1_get_opnd(ir1, 0);
    src = ir1_get_opnd(ir1, 1);
    if (!shbr_is_xmm(dest)) {
        return;
    }

    dest_mask = shbr_xmm_mask(dest);
    facts->def |= dest_mask;
    if (shbr_is_xmm(src)) {
        facts->dep |= dest_mask;
        if (high_bits == 32 && shbr_xmm_mask(src) != dest_mask) {
            facts->candidate |= dest_mask;
        }
    } else if (high_bits == 32 && ir1_opnd_is_mem(src)) {
        /*
         * MOVSS xmm, m32 defines the low scalar and architecturally clears
         * XMM[127:32].  If those bits are dead, load the scalar directly
         * into the destination instead of materializing the clear.
         */
        facts->candidate |= dest_mask;
    }
}

static void shbr_apply_movsd(IR1_INST *ir1, ShbrFacts *facts,
        int high_bits)
{
    IR1_OPND *dest;
    IR1_OPND *src;
    uint16_t dest_mask;

    if (ir1_get_opnd_num(ir1) < 2) {
        facts->read = SHBR_XMM_ALL;
        return;
    }
    dest = ir1_get_opnd(ir1, 0);
    src = ir1_get_opnd(ir1, 1);
    if (!shbr_is_xmm(dest)) {
        if (high_bits == 32 && shbr_is_xmm(src)) {
            facts->read |= shbr_xmm_mask(src);
        }
        return;
    }

    dest_mask = shbr_xmm_mask(dest);
    facts->def |= dest_mask;
    if (shbr_is_xmm(src)) {
        facts->dep |= dest_mask;
        if (high_bits == 32) {
            facts->dep |= shbr_xmm_mask(src);
        } else if (shbr_xmm_mask(src) != dest_mask) {
            facts->candidate |= dest_mask;
        }
    } else if (high_bits == 64 && ir1_opnd_is_mem(src)) {
        /* MOVSD xmm, m64 clears XMM[127:64], which may be omitted when dead. */
        facts->candidate |= dest_mask;
    }
}

static void shbr_apply_movd(IR1_INST *ir1, ShbrFacts *facts)
{
    if (ir1_get_opnd_num(ir1) > 0 && shbr_is_xmm(ir1_get_opnd(ir1, 0))) {
        facts->def |= shbr_xmm_mask(ir1_get_opnd(ir1, 0));
    }
}

static void shbr_apply_movq(IR1_INST *ir1, ShbrFacts *facts, int high_bits)
{
    IR1_OPND *dest;
    IR1_OPND *src;

    if (ir1_get_opnd_num(ir1) < 2) {
        facts->read = SHBR_XMM_ALL;
        return;
    }
    dest = ir1_get_opnd(ir1, 0);
    src = ir1_get_opnd(ir1, 1);
    if (shbr_is_xmm(dest)) {
        facts->def |= shbr_xmm_mask(dest);
        if (high_bits == 32 && shbr_is_xmm(src)) {
            facts->dep |= shbr_xmm_mask(src);
        }
    } else if (high_bits == 32 && shbr_is_xmm(src)) {
        facts->read |= shbr_xmm_mask(src);
    }
}

static bool shbr_scalar_candidate_supported(IR1_INST *ir1)
{
    switch (ir1_opcode(ir1)) {
    case X86_INS_ADDSS:
    case X86_INS_ADDSD:
    case X86_INS_SUBSS:
    case X86_INS_SUBSD:
    case X86_INS_MULSS:
    case X86_INS_MULSD:
    case X86_INS_DIVSS:
    case X86_INS_DIVSD:
    case X86_INS_MAXSS:
    case X86_INS_MAXSD:
    case X86_INS_MINSS:
    case X86_INS_MINSD:
    case X86_INS_SQRTSS:
    case X86_INS_SQRTSD:
    case X86_INS_RCPSS:
    case X86_INS_RSQRTSS:
    case X86_INS_ROUNDSS:
    case X86_INS_ROUNDSD:
    case X86_INS_CVTSD2SS:
    case X86_INS_CVTSS2SD:
    case X86_INS_CMPEQSS:
    case X86_INS_CMPLTSS:
    case X86_INS_CMPLESS:
    case X86_INS_CMPUNORDSS:
    case X86_INS_CMPNEQSS:
    case X86_INS_CMPNLTSS:
    case X86_INS_CMPNLESS:
    case X86_INS_CMPORDSS:
    case X86_INS_CMPEQSD:
    case X86_INS_CMPLTSD:
    case X86_INS_CMPLESD:
    case X86_INS_CMPUNORDSD:
    case X86_INS_CMPNEQSD:
    case X86_INS_CMPNLTSD:
    case X86_INS_CMPNLESD:
    case X86_INS_CMPORDSD:
        return true;
    default:
        return false;
    }
}

static int shbr_scalar64_direct_read_first(IR1_INST *ir1)
{
    switch (ir1_opcode(ir1)) {
    case X86_INS_SQRTSD:
    case X86_INS_ROUNDSD:
        return 1;
    case X86_INS_CVTSS2SD:
        return -1;
    default:
        return 0;
    }
}

static void shbr_apply_scalar(IR1_INST *ir1, ShbrFacts *facts,
        int high_bits, int scalar_bits, bool source_high32)
{
    IR1_OPND *dest;
    IR1_OPND *src;
    uint16_t dest_mask;

    if (ir1_get_opnd_num(ir1) < 2) {
        facts->read = SHBR_XMM_ALL;
        return;
    }
    dest = ir1_get_opnd(ir1, 0);
    src = ir1_get_opnd(ir1, 1);
    if (!shbr_is_xmm(dest)) {
        facts->read = SHBR_XMM_ALL;
        return;
    }

    dest_mask = shbr_xmm_mask(dest);
    facts->def |= dest_mask;
    facts->dep |= dest_mask;

    if (high_bits == scalar_bits && shbr_scalar_candidate_supported(ir1)) {
        facts->candidate |= dest_mask;
    }

    if (high_bits == 32 && scalar_bits == 64) {
        int direct_read_first = shbr_scalar64_direct_read_first(ir1);

        if (source_high32 && shbr_is_xmm(src)) {
            facts->dep |= shbr_xmm_mask(src);
        }
        /*
         * A double-precision input is not bit-lane independent: bits 63:32
         * can affect even result bits 31:0 and the MXCSR status.  Therefore
         * this is a direct read, not merely a dependency of live high bits.
         */
        if (direct_read_first >= 0) {
            facts->read |= shbr_explicit_xmm_mask(ir1,
                                                  direct_read_first);
        }
    }
}

static void shbr_apply_cvtsd2ss(IR1_INST *ir1, ShbrFacts *facts,
        int high_bits)
{
    shbr_apply_scalar(ir1, facts, high_bits, 32, false);

    /* Source bits 63:32 contribute to the architecturally visible result. */
    if (high_bits == 32 && ir1_get_opnd_num(ir1) > 1 &&
        shbr_is_xmm(ir1_get_opnd(ir1, 1))) {
        facts->read |= shbr_xmm_mask(ir1_get_opnd(ir1, 1));
    }
}

static void shbr_apply_cvtss2sd(IR1_INST *ir1, ShbrFacts *facts,
        int high_bits)
{
    /* The single-precision source has no tracked bits above bit 31. */
    shbr_apply_scalar(ir1, facts, high_bits, 64, false);
}

static uint16_t shbr_implicit_xmm_read_mask(IR1_INST *ir1)
{
    switch (ir1_opcode(ir1)) {
    case X86_INS_BLENDVPD:
    case X86_INS_BLENDVPS:
    case X86_INS_PBLENDVB:
    case X86_INS_SHA256RNDS2:
        return 1U; /* Legacy encodings read XMM0 implicitly. */
    default:
        return 0;
    }
}

static IR1_OPND *shbr_vector_dest(IR1_INST *ir1)
{
    int opnd_num = ir1_get_opnd_num(ir1);

    for (int i = 0; i < opnd_num; i++) {
        IR1_OPND *opnd = ir1_get_opnd(ir1, i);

        if (shbr_is_vector(opnd) && (opnd->access & CS_AC_WRITE)) {
            return opnd;
        }
    }
    return NULL;
}

static IR1_OPND *shbr_first_read_vector(IR1_INST *ir1, int first)
{
    int opnd_num = ir1_get_opnd_num(ir1);

    for (int i = first; i < opnd_num; i++) {
        IR1_OPND *opnd = ir1_get_opnd(ir1, i);

        if (shbr_is_vector(opnd) && (opnd->access & CS_AC_READ)) {
            return opnd;
        }
    }
    return NULL;
}

static bool shbr_apply_access(IR1_INST *ir1, ShbrFacts *facts,
        bool direct_reads)
{
    uint16_t dependencies = 0;
    uint16_t definitions = 0;
    uint16_t address_reads = 0;
    bool saw_vector_access = false;
    int opnd_num = ir1_get_opnd_num(ir1);

    for (int i = 0; i < opnd_num; i++) {
        IR1_OPND *opnd = ir1_get_opnd(ir1, i);

        if (shbr_has_vector_index(opnd)) {
            address_reads |= shbr_vector_index_mask(opnd);
            saw_vector_access = true;
        }
        if (shbr_is_vector(opnd)) {
            uint16_t mask = shbr_vector_mask(opnd);

            if (opnd->access & CS_AC_READ) {
                dependencies |= mask;
                saw_vector_access = true;
            }
            if (opnd->access & CS_AC_WRITE) {
                definitions |= mask;
                saw_vector_access = true;
            }
        }
    }

    if (!saw_vector_access) {
        return false;
    }
    if (!definitions) {
        facts->read |= dependencies | address_reads;
        return true;
    }

    facts->def |= definitions;
    facts->dep |= dependencies;
    facts->read |= address_reads;
    if (direct_reads) {
        facts->read |= dependencies;
    }
    return true;
}

static bool shbr_apply_dest(IR1_INST *ir1, ShbrFacts *facts)
{
    IR1_OPND *dest = shbr_vector_dest(ir1);
    uint16_t mask;

    if (!dest) {
        return false;
    }
    mask = shbr_vector_mask(dest);
    facts->def |= mask;
    facts->dep |= mask;
    return true;
}

static bool shbr_apply_other(IR1_INST *ir1, ShbrFacts *facts)
{
    IR1_OPND *dest = shbr_vector_dest(ir1);

    if (!dest) {
        return false;
    }
    facts->def |= shbr_vector_mask(dest);
    return true;
}

static bool shbr_apply_broadcast(IR1_INST *ir1, ShbrFacts *facts,
        int high_bits)
{
    IR1_OPND *dest = shbr_vector_dest(ir1);
    IR1_OPND *source;
    uint16_t dest_mask;

    if (!dest || ir1_opcode(ir1) != X86_INS_MOVDDUP) {
        return false;
    }
    source = shbr_first_read_vector(ir1, 1);
    dest_mask = shbr_vector_mask(dest);
    facts->def |= dest_mask;
    if (source && high_bits == 32) {
        facts->dep |= shbr_vector_mask(source);
    }
    return true;
}

static int shbr_extract_element_bits(IR1_INST *ir1)
{
    switch (ir1_opcode(ir1)) {
    case X86_INS_PEXTRB:
        return 8;
    case X86_INS_PEXTRW:
        return 16;
    case X86_INS_EXTRACTPS:
    case X86_INS_PEXTRD:
        return 32;
    case X86_INS_PEXTRQ:
        return 64;
    default:
        return 0;
    }
}

static bool shbr_apply_extract(IR1_INST *ir1, ShbrFacts *facts,
        int high_bits)
{
    int opnd_num = ir1_get_opnd_num(ir1);
    int element_bits = shbr_extract_element_bits(ir1);
    IR1_OPND *source;
    unsigned int elements;
    unsigned int selected;

    if (!element_bits || opnd_num < 3 ||
        !ir1_opnd_is_imm(ir1_get_opnd(ir1, opnd_num - 1))) {
        return false;
    }
    source = shbr_first_read_vector(ir1, 0);
    if (!source) {
        return false;
    }
    elements = 128 / element_bits;
    selected = ir1_opnd_uimm(ir1_get_opnd(ir1, opnd_num - 1)) &
               (elements - 1);
    if ((selected + 1) * element_bits > (unsigned int)high_bits) {
        facts->read |= shbr_vector_mask(source);
    }
    return true;
}

static bool shbr_apply_explicit(IR1_INST *ir1, ShbrFacts *facts,
        ShbrRule rule, int high_bits)
{
    switch (rule) {
    case SHBR_RULE_ACCESS:
        return shbr_apply_access(ir1, facts, false);
    case SHBR_RULE_ACCESS_READ:
        return shbr_apply_access(ir1, facts, true);
    case SHBR_RULE_DEST:
        return shbr_apply_dest(ir1, facts);
    case SHBR_RULE_READ_ACCESS:
        return shbr_apply_access(ir1, facts, true);
    case SHBR_RULE_OTHER:
        return shbr_apply_other(ir1, facts);
    case SHBR_RULE_IGNORE:
        return true;
    case SHBR_RULE_MOVQ:
        shbr_apply_movq(ir1, facts, high_bits);
        return true;
    case SHBR_RULE_BROADCAST:
        return shbr_apply_broadcast(ir1, facts, high_bits);
    case SHBR_RULE_EXTRACT:
        return shbr_apply_extract(ir1, facts, high_bits);
    case SHBR_RULE_IMPLICIT:
        facts->read = SHBR_XMM_ALL;
        return true;
    default:
        return false;
    }
}

static bool shbr_manual_rule_has_priority(ShbrRule rule)
{
    switch (rule) {
    case SHBR_RULE_MOVSS:
    case SHBR_RULE_MOVSD:
    case SHBR_RULE_MOVD:
    case SHBR_RULE_MOVQ:
    case SHBR_RULE_SCALAR32:
    case SHBR_RULE_SCALAR64:
    case SHBR_RULE_CVTSD2SS:
    case SHBR_RULE_CVTSS2SD:
        return true;
    default:
        return false;
    }
}

static bool shbr_is_plain_integer(IR1_INST *ir1)
{
    /*
     * These translations only change integer registers/flags.  Memory exits
     * are accounted for separately below.  Do not infer transparency merely
     * from the absence of XMM operands: privileged/implicit state operations,
     * divide exceptions and locked helper paths must remain barriers.
     */
    if (ir1_is_prefix_lock(ir1)) {
        return false;
    }
    switch (ir1_opcode(ir1)) {
    case X86_INS_MOV:
    case X86_INS_MOVABS:
    case X86_INS_MOVSX:
    case X86_INS_MOVSXD:
    case X86_INS_MOVZX:
    case X86_INS_LEA:
    case X86_INS_ADD:
    case X86_INS_ADC:
    case X86_INS_SUB:
    case X86_INS_SBB:
    case X86_INS_CMP:
    case X86_INS_INC:
    case X86_INS_DEC:
    case X86_INS_NEG:
    case X86_INS_AND:
    case X86_INS_OR:
    case X86_INS_XOR:
    case X86_INS_TEST:
    case X86_INS_NOT:
        break;
    default:
        return false;
    }

    int opnd_num = ir1_get_opnd_num(ir1);

    if (opnd_num == 0) {
        return false;
    }
    for (int i = 0; i < opnd_num; i++) {
        IR1_OPND *opnd = ir1_get_opnd(ir1, i);

        /* In particular MOV to/from segment, control and debug registers. */
        if (!ir1_opnd_is_gpr(opnd) && !ir1_opnd_is_imm(opnd) &&
            !ir1_opnd_is_mem(opnd)) {
            return false;
        }
        if (shbr_has_vector_index(opnd)) {
            return false;
        }
    }
    return true;
}

static ShbrFacts shbr_get_facts(IR1_INST *ir1, int high_bits)
{
    ShbrFacts facts = { 0 };
    ShbrRule rule = shbr_rule(ir1);
    ShbrRule explicit_rule = high_bits == 32 ?
        shbr_explicit_semantics[ir1_opcode(ir1)].high32 :
        shbr_explicit_semantics[ir1_opcode(ir1)].high64;

    if (shbr_is_plain_integer(ir1)) {
        /* LEA computes an address without performing a memory access. */
        if (ir1_opcode(ir1) == X86_INS_LEA) {
            return facts;
        }
        goto memory_exit;
    }

    if (!shbr_manual_rule_has_priority(rule) &&
        explicit_rule != SHBR_RULE_NONE) {
        if (!shbr_apply_explicit(ir1, &facts, explicit_rule, high_bits)) {
            facts.read = SHBR_XMM_ALL;
        }
        goto memory_exit;
    }

    /*
     * Several packed-integer opcodes also have MMX forms.  They share the
     * opcode but not XMM state, and can still exit through the x87/MMX
     * exception path before a later XMM overwrite.  Keep such forms as full
     * barriers instead of applying the XMM data-flow rule to an empty mask.
     */
    if (rule != SHBR_RULE_BARRIER &&
        shbr_explicit_xmm_mask(ir1, 0) == 0) {
        facts.read = SHBR_XMM_ALL;
        return facts;
    }

    switch (rule) {
    case SHBR_RULE_FULL_MOVE:
        shbr_apply_full_move(ir1, &facts);
        break;
    case SHBR_RULE_RMW:
        shbr_apply_rmw(ir1, &facts, false);
        break;
    case SHBR_RULE_RMW_READ:
        shbr_apply_rmw(ir1, &facts, true);
        break;
    case SHBR_RULE_RMW_BARRIER:
        shbr_apply_rmw(ir1, &facts, false);
        facts.read = SHBR_XMM_ALL;
        break;
    case SHBR_RULE_READ:
        facts.read |= shbr_explicit_xmm_mask(ir1, 0);
        break;
    case SHBR_RULE_MOVSS:
        shbr_apply_movss(ir1, &facts, high_bits);
        break;
    case SHBR_RULE_MOVSD:
        shbr_apply_movsd(ir1, &facts, high_bits);
        break;
    case SHBR_RULE_MOVD:
        shbr_apply_movd(ir1, &facts);
        break;
    case SHBR_RULE_MOVQ:
        shbr_apply_movq(ir1, &facts, high_bits);
        break;
    case SHBR_RULE_SCALAR32:
        shbr_apply_scalar(ir1, &facts, high_bits, 32, false);
        break;
    case SHBR_RULE_SCALAR64:
        shbr_apply_scalar(ir1, &facts, high_bits, 64, true);
        break;
    case SHBR_RULE_CVTSD2SS:
        shbr_apply_cvtsd2ss(ir1, &facts, high_bits);
        break;
    case SHBR_RULE_CVTSS2SD:
        shbr_apply_cvtss2sd(ir1, &facts, high_bits);
        break;
    case SHBR_RULE_BARRIER:
    default:
        facts.read = SHBR_XMM_ALL;
        break;
    }

memory_exit:
    facts.read |= shbr_implicit_xmm_read_mask(ir1);

    /*
     * A fault or MMIO/helper exit can materialize every old XMM register.
     * A data watchpoint is different: x86 reports it after the memory access,
     * so the debug handler can inspect the current instruction's complete XMM
     * result too.  A memory access therefore remains a liveness barrier for
     * earlier instructions.  The current scalar instruction may still be a
     * candidate: its memory access and SIMD exception checks complete before
     * the destination is modified, so every faulting path retains the old
     * architectural XMM value.  An after-access watchpoint re-executes the
     * instruction in a one-instruction TB, whose live-out state rejects SHBR.
     */
    if (shbr_has_memory_operand(ir1) ||
        shbr_has_implicit_memory_access(ir1)) {
        if (!latxs_shbr_ignore_memory()) {
            facts.read |= SHBR_XMM_ALL;
        }
        facts.memory_barrier = true;
    }
    return facts;
}

static bool shbr_tb_can_analyze(const TranslationBlock *tb);

static bool shbr_tu_has_zero_def64(uint32_t tb_num,
        TranslationBlock **tb_list, bool user_only);

static uint16_t shbr_known_zero_def64(IR1_INST *ir1)
{
    IR1_OPND *dest;

    if (ir1_get_opnd_num(ir1) < 2) {
        return 0;
    }
    dest = ir1_get_opnd(ir1, 0);
    if (!shbr_is_xmm(dest)) {
        return 0;
    }

    switch (ir1_opcode(ir1)) {
    case X86_INS_MOVD:
    case X86_INS_MOVQ:
        return shbr_xmm_mask(dest);
    case X86_INS_MOVSD:
    case X86_INS_MOVSS:
        return ir1_opnd_is_mem(ir1_get_opnd(ir1, 1)) ?
               shbr_xmm_mask(dest) : 0;
    default:
        break;
    }

    if (shbr_is_self_zeroing(ir1) &&
        shbr_is_xmm(ir1_get_opnd(ir1, 1)) &&
        shbr_xmm_mask(dest) == shbr_xmm_mask(ir1_get_opnd(ir1, 1))) {
        return shbr_xmm_mask(dest);
    }
    return 0;
}

static bool shbr_known_zero_clobbers_all(IR1_INST *ir1)
{
    switch (ir1_opcode(ir1)) {
    case X86_INS_FXRSTOR:
    case X86_INS_FXRSTOR64:
    case X86_INS_XRSTOR:
        return true;
    default:
        /* An unmodeled instruction may write XMM state implicitly. */
        return shbr_rule(ir1) == SHBR_RULE_BARRIER &&
               shbr_explicit_semantics[ir1_opcode(ir1)].high64 ==
                   SHBR_RULE_NONE &&
               !shbr_is_plain_integer(ir1);
    }
}

static uint16_t shbr_known_zero_after(IR1_INST *ir1, ShbrFacts facts,
                                     uint16_t known_zero)
{
    uint16_t zero_def = shbr_known_zero_def64(ir1);

    if (shbr_known_zero_clobbers_all(ir1)) {
        return 0;
    }
    if (shbr_rule(ir1) == SHBR_RULE_FULL_MOVE &&
        ir1_get_opnd_num(ir1) == 2 &&
        shbr_is_xmm(ir1_get_opnd(ir1, 0)) &&
        shbr_is_xmm(ir1_get_opnd(ir1, 1)) &&
        (known_zero & shbr_xmm_mask(ir1_get_opnd(ir1, 1)))) {
        zero_def |= shbr_xmm_mask(ir1_get_opnd(ir1, 0));
    }
    return (known_zero & ~(facts.def & ~facts.candidate)) | zero_def;
}

static void shbr_known_zero_finish(IR1_INST *ir1_list, int ir1_num,
        uint16_t pending_at[MAX_IR1_NUM_PER_TB], uint8_t count[16],
        uint16_t mask,
        bool needs_restore, IR1_INST *restore_ir1, bool restore_after,
        bool collect_stats)
{
    uint64_t new_hits = 0;
    uint16_t restore_mask = 0;

    while (mask) {
        int reg = ctz32(mask);
        uint16_t bit = 1U << reg;
        unsigned int new_candidates = 0;

        mask &= mask - 1;
        for (int i = 0; i < ir1_num; i++) {
            if ((pending_at[i] & bit) &&
                !(ir1_list[i].shbr_flags & LATXS_SHBR_HIGH64)) {
                new_candidates++;
            }
        }
        if (new_candidates > (needs_restore ? 1 : 0)) {
            for (int i = 0; i < ir1_num; i++) {
                if (pending_at[i] & bit) {
                    ir1_list[i].shbr_flags |= LATXS_SHBR_HIGH64;
                }
            }
            new_hits += new_candidates;
            if (needs_restore) {
                restore_mask |= bit;
            }
        }
        for (int i = 0; i < ir1_num; i++) {
            pending_at[i] &= ~bit;
        }
        count[reg] = 0;
    }

    if (restore_mask) {
        g_assert(restore_ir1);
        if (restore_after) {
            restore_ir1->shbr_restore_zero64_after |= restore_mask;
        } else {
            restore_ir1->shbr_restore_zero64_before |= restore_mask;
        }
    }
    if (collect_stats) {
        SHBR_STATS_ADD(known_zero_new_hits64, new_hits);
        SHBR_STATS_ADD(known_zero_restores64,
                       __builtin_popcount((unsigned int)restore_mask));
    }
}

static void shbr_analyze_known_zero_tb(TranslationBlock *tb,
        uint32_t cpuid_ext_features, bool collect_stats)
{
    IR1_INST *ir1_list = tb->s_data->ir1;
    uint16_t pending_at[MAX_IR1_NUM_PER_TB] = { 0 };
    uint8_t count[16] = { 0 };
    uint16_t known_zero = 0;
    uint16_t pending = 0;
    int ir1_num = tb->icount;

    if (!shbr_tb_can_analyze(tb) || ir1_num <= 0) {
        return;
    }

    for (int i = 0; i < ir1_num; i++) {
        IR1_INST *ir1 = &ir1_list[i];
        bool direct_terminator = i == ir1_num - 1 &&
            (tb->s_data->last_ir1_type == IR1_TYPE_BRANCH ||
             tb->s_data->last_ir1_type == IR1_TYPE_JUMP);

        if (direct_terminator) {
            shbr_known_zero_finish(ir1_list, ir1_num, pending_at, count,
                                   pending, true, ir1, false,
                                   collect_stats);
            return;
        }

        ShbrFacts facts = shbr_get_facts(ir1, 64);
        if (shbr_missing_cpuid_feature(ir1, cpuid_ext_features) ||
            (option_sigfpe && (tb->flags & HF_SIGFPE_ON_MASK) &&
             shbr_has_vector_operand(ir1))) {
            facts.read |= SHBR_XMM_ALL;
            facts.candidate = 0;
        }

        uint16_t observed = pending &
            (facts.read | (facts.dep & ~facts.candidate));
        shbr_known_zero_finish(ir1_list, ir1_num, pending_at, count,
                               observed, true, ir1, false, collect_stats);
        pending &= ~observed;

        uint16_t recover_candidate = 0;

        if (ir1->shbr_flags & LATXS_SHBR_RECOVER_ZERO64) {
            /*
             * The shared successor TB may also be entered from the dispatcher
             * with arbitrary XMM state.  Therefore a cross-TB candidate must
             * establish its zero premise inside this TB, before the skipped
             * high-half write.  It is kept out of the local pending run; the
             * TU-wide recovery pass assigns restoration to the precise cold
             * edge instead.
             */
            if (!(ir1->shbr_flags & LATXS_SHBR_HIGH64) &&
                facts.candidate &&
                (facts.candidate & known_zero) == facts.candidate) {
                recover_candidate = facts.candidate;
                if (collect_stats) {
                    SHBR_STATS_ADD(recover_local_zero64,
                        __builtin_popcount(
                            (unsigned int)recover_candidate));
                }
            } else {
                ir1->shbr_flags &= ~LATXS_SHBR_RECOVER_ZERO64;
            }
        }

        /* A fresh MOVSD memory load can itself start a deferred-zero run. */
        uint16_t zero_def = shbr_known_zero_def64(ir1);
        uint16_t candidate = (facts.candidate & (known_zero | zero_def)) &
                             ~recover_candidate;
        if (candidate) {
            pending_at[i] |= candidate;
            pending |= candidate;
            for (uint16_t bits = candidate; bits; bits &= bits - 1) {
                count[ctz32(bits)]++;
            }
            if (collect_stats) {
                SHBR_STATS_ADD(known_zero_candidates64,
                               __builtin_popcount((unsigned int)candidate));
            }
        }
        if (collect_stats && recover_candidate) {
            SHBR_STATS_ADD(known_zero_candidates64,
                           __builtin_popcount(
                               (unsigned int)recover_candidate));
        }

        uint16_t overwritten = facts.def & ~facts.candidate;
        uint16_t finished = pending & overwritten;
        shbr_known_zero_finish(ir1_list, ir1_num, pending_at, count,
                               finished, false, NULL, false, collect_stats);
        pending &= ~finished;
        known_zero = shbr_known_zero_after(ir1, facts, known_zero);
    }

    shbr_known_zero_finish(ir1_list, ir1_num, pending_at, count, pending,
                           true, &ir1_list[ir1_num - 1], true,
                           collect_stats);
}

static void shbr_analyze_known_zero_tu(uint32_t tb_num,
        TranslationBlock **tb_list, uint32_t cpuid_ext_features,
        bool collect_stats)
{
    /*
     * The known-zero pass can only create a deferred-zero run after an
     * instruction explicitly establishes a zero high half.  If none exists,
     * skip the local pass in both guest user and kernel code.
     */
    if (!shbr_tu_has_zero_def64(tb_num, tb_list, false)) {
        return;
    }
    for (uint32_t i = 0; i < tb_num; i++) {
        shbr_analyze_known_zero_tb(tb_list[i], cpuid_ext_features,
                                   collect_stats);
    }
}

static bool shbr_tb_can_analyze(const TranslationBlock *tb)
{
    /* #NM/#UD and post-instruction #DB can expose complete XMM state. */
    return !(tb->flags & (HF_TS_MASK | HF_EM_MASK | HF_TF_MASK)) &&
           !(tb_cflags(tb) & CF_SINGLE_STEP) &&
           (tb->flags & HF_OSFXSR_MASK);
}

static bool shbr_tu_may_have_candidate32(uint32_t tb_num,
        TranslationBlock **tb_list)
{
    /*
     * Only these manual rules can set facts.candidate for high_bits == 32.
     * Avoid a complete CFG fixed point when a TU contains none of them.
     */
    for (uint32_t i = 0; i < tb_num; i++) {
        TranslationBlock *tb = tb_list[i];
        IR1_INST *ir1_list = tb->s_data->ir1;

        if (!shbr_tb_can_analyze(tb)) {
            continue;
        }
        for (int j = 0; j < tb->icount; j++) {
            switch (shbr_rule(&ir1_list[j])) {
            case SHBR_RULE_MOVSS:
            case SHBR_RULE_SCALAR32:
            case SHBR_RULE_CVTSD2SS:
                return true;
            default:
                break;
            }
        }
    }
    return false;
}

static uint16_t shbr_scan_block(TranslationBlock *tb, uint16_t live,
        int high_bits, bool mark_candidates, uint32_t cpuid_ext_features,
        bool collect_stats, bool recover_zero)
{
    IR1_INST *ir1_list = tb->s_data->ir1;
    uint8_t flag = recover_zero ? LATXS_SHBR_RECOVER_ZERO64 :
                   high_bits == 32 ? LATXS_SHBR_HIGH32 : LATXS_SHBR_HIGH64;

    if (!shbr_tb_can_analyze(tb)) {
        return SHBR_XMM_ALL;
    }

    for (int i = tb->icount - 1; i >= 0; i--) {
        IR1_INST *ir1 = &ir1_list[i];
        bool direct_terminator = i == tb->icount - 1 &&
            (tb->s_data->last_ir1_type == IR1_TYPE_BRANCH ||
             tb->s_data->last_ir1_type == IR1_TYPE_JUMP);
        ShbrFacts facts = { 0 };

        if (!direct_terminator) {
            facts = shbr_get_facts(ir1, high_bits);

            /*
             * The system translator raises #UD before modifying XMM state
             * when an opcode's required CPUID feature is absent.  Preserve
             * all pre-instruction XMM values for that architectural exit.
             */
            if (shbr_missing_cpuid_feature(ir1, cpuid_ext_features)) {
                facts.read |= SHBR_XMM_ALL;
                facts.candidate = 0;
            }

            /*
             * With an unmasked SIMD exception, the guest handler can inspect
             * every pre-instruction XMM register.  The candidate instruction
             * itself remains safe because its faulting result is staged until
             * after the exception check; this fence protects earlier writes.
             */
            if (option_sigfpe && (tb->flags & HF_SIGFPE_ON_MASK) &&
                shbr_has_vector_operand(ir1)) {
                facts.read |= SHBR_XMM_ALL;
            }

            /*
             * A recoverable value may cross a loop backedge only while a
             * scalar SHBR candidate carries the same architectural zero.
             * Treat every other high-half dependency as an observation so a
             * packed transform cannot create a value that a cold exit could
             * no longer reconstruct by writing zero.
             */
            if (recover_zero) {
                facts.read |= facts.dep & ~facts.candidate;
                facts.dep &= facts.candidate;
            }

            if (collect_stats && !recover_zero) {
                uint16_t accepted = facts.candidate & ~live;
                unsigned int candidates =
                    __builtin_popcount((unsigned int)facts.candidate);
                unsigned int hits =
                    __builtin_popcount((unsigned int)accepted);

                if (high_bits == 32) {
                    SHBR_STATS_ADD(candidates32, candidates);
                    SHBR_STATS_ADD(hits32, hits);
                    SHBR_STATS_ADD(rejected_live32, candidates - hits);
                } else {
                    SHBR_STATS_ADD(ir1s, 1);
                    SHBR_STATS_ADD(memory_barriers, facts.memory_barrier);
                    SHBR_STATS_ADD(candidates64, candidates);
                    SHBR_STATS_ADD(hits64, hits);
                    SHBR_STATS_ADD(rejected_live64, candidates - hits);
                }
            }
        }

        if (mark_candidates && facts.candidate &&
            !(live & facts.candidate) &&
            (!recover_zero ||
             !(ir1->shbr_flags & LATXS_SHBR_HIGH64))) {
            ir1->shbr_flags |= flag;
            if (collect_stats && recover_zero) {
                SHBR_STATS_ADD(recover_tentative64,
                    __builtin_popcount((unsigned int)facts.candidate));
            }
        }
        live = latxs_shbr_live_before(live, facts.read,
                                      facts.def, facts.dep);
    }
    return live;
}

static int shbr_tb_index(uint32_t tb_num, TranslationBlock **tb_list,
        TranslationBlock *tb)
{
    for (uint32_t i = 0; tb && i < tb_num; i++) {
        if (tb_list[i] == tb) {
            return i;
        }
    }
    return -1;
}

static bool shbr_relax_edge(bool collect_stats, bool recover_zero)
{
    if (recover_zero || !latxs_shbr_aggressive()) {
        return false;
    }
    if (collect_stats) {
        SHBR_STATS_ADD(edges_aggressive, 1);
    }
    return true;
}

/*
 * An in-TU pointer is not sufficient on its own.  Some system-mode edges
 * still leave translated code to check interrupts or validate another page.
 * State may be materialized on those exits before the successor overwrites
 * it, so only edges emitted as uninterrupted TU control flow may propagate
 * liveness.
 */
static inline int QEMU_ALWAYS_INLINE shbr_check_successor(
        TranslationBlock *tb, TranslationBlock *successor,
        int tb_index, int successor_index, int successor_id,
        bool collect_stats, bool recover_zero)
{
    if (tb_index < 0 || successor_index < 0) {
        if (collect_stats) {
            SHBR_STATS_ADD(edges_outside_tu, 1);
        }
        return -1;
    }
    if (!latxs_check_interrupt_at_block_end()) {
        if (shbr_relax_edge(collect_stats, recover_zero)) {
            return successor_index;
        }
        if (collect_stats) {
            SHBR_STATS_ADD(edges_interrupt_mode, 1);
        }
        return -1;
    }
    if ((tb->flags & HF_TF_MASK) ||
        tb->s_data->sys_eob_ir1_index >= 0 ||
        (tb_cflags(tb) & (CF_NO_GOTO_TB | CF_NO_GOTO_PTR |
                          CF_SINGLE_STEP | CF_USE_ICOUNT))) {
        if (shbr_relax_edge(collect_stats, recover_zero)) {
            return successor_index;
        }
        if (collect_stats) {
            SHBR_STATS_ADD(edges_tb_state, 1);
        }
        return -1;
    }

    /*
     * Backward TU edges perform the block-end interrupt check.  The normal
     * analysis stops there because the exit can observe complete XMM state.
     * The zero-recovery analysis may cross only a direct target backedge: its
     * cold path has edge-specific code where zero can be materialized before
     * leaving translated code.
     */
    if (successor->pc <= tb->pc) {
        bool recoverable = recover_zero && !in_aot_translate &&
            !option_aot_save && successor_id == TU_TB_INDEX_TARGET &&
            ((tb->flags & HF_CPL_MASK) >> HF_CPL_SHIFT) == 3 &&
            ((successor->flags & HF_CPL_MASK) >> HF_CPL_SHIFT) == 3 &&
            (tb->s_data->last_ir1_type == IR1_TYPE_BRANCH ||
             tb->s_data->last_ir1_type == IR1_TYPE_JUMP);

        if (!recoverable) {
            if (shbr_relax_edge(collect_stats, recover_zero)) {
                return successor_index;
            }
            if (collect_stats) {
                SHBR_STATS_ADD(edges_backward, 1);
            }
            return -1;
        }
    }

    /* A non-fast cross-page edge can leave through the page-check path. */
    if (!tucp_fast_tu_direct() &&
        (successor->pc & TARGET_PAGE_MASK) !=
        (tb->pc & TARGET_PAGE_MASK)) {
        if (shbr_relax_edge(collect_stats, recover_zero)) {
            return successor_index;
        }
        if (collect_stats) {
            SHBR_STATS_ADD(edges_cross_page, 1);
        }
        return -1;
    }

    if (tb->s_data->last_ir1_type == IR1_TYPE_NORMAL) {
        /*
         * The only state-preserving normal edge is an adjacent fall-through
         * created because the source TB hit the instruction-count limit.
         * A system EOB (page boundary, breakpoint, inhibit-IRQ handling,
         * etc.) materializes architectural state even when its successor is
         * adjacent.  A non-adjacent too-large successor uses the same EOB
         * path.  Profiling builds also force that path for every normal TB.
         */
#ifdef CONFIG_LATX_PROFILER
        if (shbr_relax_edge(collect_stats, recover_zero)) {
            return successor_index;
        }
        if (collect_stats) {
            SHBR_STATS_ADD(edges_normal_eob, 1);
        }
        return -1;
#else
        if (tb->s_data->tb_too_large_ir1_index < 0 ||
            successor_index != tb_index + 1) {
            if (shbr_relax_edge(collect_stats, recover_zero)) {
                return successor_index;
            }
            if (collect_stats) {
                SHBR_STATS_ADD(edges_normal_eob, 1);
            }
            return -1;
        }
#endif
    }

    if (collect_stats) {
        SHBR_STATS_ADD(edges_safe, 1);
    }
    return successor_index;
}

static int shbr_safe_successor_index(uint32_t tb_num,
        TranslationBlock **tb_list, TranslationBlock *tb, int successor_id,
        bool collect_stats, bool recover_zero)
{
    TranslationBlock *successor = tb->s_data->next_tb[successor_id];
    int tb_index = shbr_tb_index(tb_num, tb_list, tb);
    int successor_index = shbr_tb_index(tb_num, tb_list, successor);

    return shbr_check_successor(tb, successor, tb_index, successor_index,
                                 successor_id, collect_stats, recover_zero);
}

static uint16_t shbr_successor_live_in(uint32_t tb_num,
        TranslationBlock **tb_list, ShbrTbState *state,
        TranslationBlock *tb, int successor_id, bool recover_zero,
        bool *has_recoverable_backedge)
{
    TranslationBlock *successor = tb->s_data->next_tb[successor_id];
    int successor_index = shbr_safe_successor_index(tb_num, tb_list, tb,
                                                    successor_id, false,
                                                    recover_zero);

    /*
     * Reuse the normal liveness edge walk as the cheap recovery prefilter.
     * Only a backward edge needs the stricter recovery-specific check, so
     * loop-free TUs do not pay for another O(TB) successor scan.
     */
    if (has_recoverable_backedge && !*has_recoverable_backedge &&
        successor && successor->pc <= tb->pc &&
        shbr_safe_successor_index(tb_num, tb_list, tb, successor_id,
                                  false, true) >= 0) {
        *has_recoverable_backedge = true;
    }

    return successor_index < 0 ? SHBR_XMM_ALL :
                                 state[successor_index].live_in;
}

static void shbr_collect_edge_stats(uint32_t tb_num,
        TranslationBlock **tb_list)
{
    for (uint32_t i = 0; i < tb_num; i++) {
        TranslationBlock *tb = tb_list[i];

        switch (tb->s_data->last_ir1_type) {
        case IR1_TYPE_BRANCH:
            shbr_safe_successor_index(tb_num, tb_list, tb,
                                      TU_TB_INDEX_NEXT, true, false);
            shbr_safe_successor_index(tb_num, tb_list, tb,
                                      TU_TB_INDEX_TARGET, true, false);
            break;
        case IR1_TYPE_JUMP:
            shbr_safe_successor_index(tb_num, tb_list, tb,
                                      TU_TB_INDEX_TARGET, true, false);
            break;
        case IR1_TYPE_NORMAL:
            shbr_safe_successor_index(tb_num, tb_list, tb,
                                      TU_TB_INDEX_NEXT, true, false);
            break;
        default:
            break;
        }
    }
}

static uint16_t shbr_block_live_out(uint32_t tb_num,
        TranslationBlock **tb_list, ShbrTbState *state, TranslationBlock *tb,
        bool recover_zero, bool *has_recoverable_backedge)
{
    switch (tb->s_data->last_ir1_type) {
    case IR1_TYPE_BRANCH:
        return shbr_successor_live_in(tb_num, tb_list, state, tb,
                                     TU_TB_INDEX_NEXT, recover_zero,
                                     has_recoverable_backedge) |
               shbr_successor_live_in(tb_num, tb_list, state, tb,
                                     TU_TB_INDEX_TARGET, recover_zero,
                                     has_recoverable_backedge);
    case IR1_TYPE_JUMP:
        return shbr_successor_live_in(tb_num, tb_list, state, tb,
                                     TU_TB_INDEX_TARGET, recover_zero,
                                     has_recoverable_backedge);
    case IR1_TYPE_NORMAL:
        return shbr_successor_live_in(tb_num, tb_list, state, tb,
                                     TU_TB_INDEX_NEXT, recover_zero,
                                     has_recoverable_backedge);
    case IR1_TYPE_CALL:
    case IR1_TYPE_CALLIN:
    case IR1_TYPE_JUMPIN:
    case IR1_TYPE_RET:
    case IR1_TYPE_SYSCALL:
    case IR1_TYPE_EOB:
    case IR1_TYPE_SPLITED:
    default:
        return SHBR_XMM_ALL;
    }
}

static int shbr_successor_ids(const TranslationBlock *tb, int ids[2]);

static inline int QEMU_ALWAYS_INLINE shbr_cached_successor_index(
        uint32_t tb_num, TranslationBlock **tb_list,
        ShbrSuccessors *successors, uint32_t index, int successor_id,
        bool recover_zero)
{
    int *entry = &successors[index][recover_zero][successor_id];

    if (!*entry) {
        TranslationBlock *tb = tb_list[index];
        TranslationBlock *next = tb->s_data->next_tb[successor_id];
        int next_index = tu_sorted_tb_index(tb_num, tb_list, next);

        /* The source index is already known in this HRAS-only cache. Keep
         * its sorted lookup separate from the ordinary linear query. */
        *entry = shbr_check_successor(tb, next, index, next_index,
                                       successor_id, false, recover_zero) + 2;
    }
    return *entry - 2;
}

static uint16_t shbr_cached_block_live_out(uint32_t tb_num,
        TranslationBlock **tb_list, ShbrTbState *state,
        ShbrSuccessors *successors, uint32_t index, bool recover_zero,
        bool *has_recoverable_backedge)
{
    TranslationBlock *tb = tb_list[index];
    uint16_t live_out = 0;
    int ids[2];
    int nr_ids = shbr_successor_ids(tb, ids);

    if (!nr_ids) {
        return SHBR_XMM_ALL;
    }
    for (int j = 0; j < nr_ids; j++) {
        TranslationBlock *successor = tb->s_data->next_tb[ids[j]];
        int next = shbr_cached_successor_index(tb_num, tb_list, successors,
                                               index, ids[j], recover_zero);

        if (has_recoverable_backedge && !*has_recoverable_backedge &&
            successor && successor->pc <= tb->pc &&
            shbr_cached_successor_index(tb_num, tb_list, successors,
                                         index, ids[j], true) >= 0) {
            *has_recoverable_backedge = true;
        }
        live_out |= next < 0 ? SHBR_XMM_ALL : state[next].live_in;
    }
    return live_out;
}

static void shbr_analyze_tu_high(uint32_t tb_num,
        TranslationBlock **tb_list, int high_bits,
        uint32_t cpuid_ext_features, bool collect_stats, bool recover_zero,
        bool *has_recoverable_backedge, ShbrSuccessors *successors)
{
    ShbrTbState *state = g_new0(ShbrTbState, tb_num);
    bool changed;
    bool first_pass = true;

    /* Compute the least fixed point over the TU control-flow graph. */
    do {
        changed = false;
        for (uint32_t i = tb_num; i-- > 0;) {
            TranslationBlock *tb = tb_list[i];
            uint16_t live_out, live_in;

            if (successors) {
                live_out = shbr_cached_block_live_out(tb_num, tb_list, state,
                    successors, i, recover_zero, has_recoverable_backedge);
                /* This unmarked scan is pure within one high/recovery pass.
                 * Every block is scanned initially, even if live_out is zero. */
                if (!first_pass && state[i].live_out == live_out) {
                    continue;
                }
            } else {
                live_out = shbr_block_live_out(tb_num, tb_list, state, tb,
                    recover_zero, has_recoverable_backedge);
            }
            live_in = shbr_scan_block(tb, live_out, high_bits, false,
                                       cpuid_ext_features, false, recover_zero);

            if (state[i].live_out != live_out ||
                state[i].live_in != live_in) {
                state[i].live_out = live_out;
                state[i].live_in = live_in;
                changed = true;
            }
        }
        first_pass = false;
    } while (changed);

    for (uint32_t i = 0; i < tb_num; i++) {
        shbr_scan_block(tb_list[i], state[i].live_out, high_bits, true,
                        cpuid_ext_features, collect_stats, recover_zero);
    }
    g_free(state);
}

typedef struct ShbrRecoverState {
    uint16_t dirty_in;
    uint16_t dirty_out;
} ShbrRecoverState;

static int shbr_successor_ids(const TranslationBlock *tb, int ids[2])
{
    switch (tb->s_data->last_ir1_type) {
    case IR1_TYPE_BRANCH:
        ids[0] = TU_TB_INDEX_NEXT;
        ids[1] = TU_TB_INDEX_TARGET;
        return 2;
    case IR1_TYPE_JUMP:
        ids[0] = TU_TB_INDEX_TARGET;
        return 1;
    case IR1_TYPE_NORMAL:
        ids[0] = TU_TB_INDEX_NEXT;
        return 1;
    default:
        return 0;
    }
}

static bool shbr_tu_has_zero_def64(uint32_t tb_num,
        TranslationBlock **tb_list, bool user_only)
{
    /* Cross-TB recovery is user-only; local zero runs are not. */
    for (uint32_t i = 0; i < tb_num; i++) {
        TranslationBlock *tb = tb_list[i];
        IR1_INST *ir1_list = tb->s_data->ir1;

        if (user_only &&
            ((tb->flags & HF_CPL_MASK) >> HF_CPL_SHIFT) != 3) {
            continue;
        }
        for (int j = 0; j < tb->icount; j++) {
            if (shbr_known_zero_def64(&ir1_list[j])) {
                return true;
            }
        }
    }

    return false;
}

static uint16_t shbr_recover_transfer(TranslationBlock *tb, uint16_t dirty,
        uint32_t cpuid_ext_features, uint16_t *observed)
{
    IR1_INST *ir1_list = tb->s_data->ir1;

    if (!shbr_tb_can_analyze(tb)) {
        if (observed) {
            *observed |= dirty;
        }
        return dirty;
    }

    for (int i = 0; i < tb->icount; i++) {
        IR1_INST *ir1 = &ir1_list[i];
        bool direct_terminator = i == tb->icount - 1 &&
            (tb->s_data->last_ir1_type == IR1_TYPE_BRANCH ||
             tb->s_data->last_ir1_type == IR1_TYPE_JUMP);

        dirty &= ~ir1->shbr_restore_zero64_before;
        if (!direct_terminator) {
            ShbrFacts facts = shbr_get_facts(ir1, 64);

            if (shbr_missing_cpuid_feature(ir1, cpuid_ext_features)) {
                facts.read |= SHBR_XMM_ALL;
                facts.candidate = 0;
            }
            if (option_sigfpe && (tb->flags & HF_SIGFPE_ON_MASK) &&
                shbr_has_vector_operand(ir1)) {
                facts.read |= SHBR_XMM_ALL;
            }

            if (observed) {
                *observed |= dirty &
                    (facts.read | (facts.dep & ~facts.candidate));
            }
            dirty &= ~(facts.def & ~facts.candidate);
            if (ir1->shbr_flags & LATXS_SHBR_RECOVER_ZERO64) {
                dirty |= facts.candidate;
            }
        }
        dirty &= ~ir1->shbr_restore_zero64_after;
    }
    return dirty;
}

static void shbr_clear_recover_candidates(uint32_t tb_num,
        TranslationBlock **tb_list)
{
    for (uint32_t i = 0; i < tb_num; i++) {
        TranslationBlock *tb = tb_list[i];
        IR1_INST *ir1_list = tb->s_data->ir1;

        for (int j = 0; j < tb->icount; j++) {
            ir1_list[j].shbr_flags &= ~LATXS_SHBR_RECOVER_ZERO64;
        }
    }
}

static void shbr_compute_recover_state(uint32_t tb_num,
        TranslationBlock **tb_list, uint32_t cpuid_ext_features,
        ShbrRecoverState *state, uint16_t *next_in,
        ShbrSuccessors *successors)
{
    bool changed;

    do {
        memset(next_in, 0, sizeof(*next_in) * tb_num);
        changed = false;
        for (uint32_t i = 0; i < tb_num; i++) {
            TranslationBlock *tb = tb_list[i];
            int ids[2];
            int nr_ids = shbr_successor_ids(tb, ids);
            uint16_t dirty_out = shbr_recover_transfer(
                tb, state[i].dirty_in, cpuid_ext_features, NULL);

            if (dirty_out != state[i].dirty_out) {
                state[i].dirty_out = dirty_out;
                changed = true;
            }
            for (int j = 0; j < nr_ids; j++) {
                int successor = successors ?
                    shbr_cached_successor_index(tb_num, tb_list, successors,
                                                  i, ids[j], true) :
                    shbr_safe_successor_index(tb_num, tb_list, tb, ids[j],
                                               false, true);

                if (successor >= 0) {
                    next_in[successor] |= dirty_out;
                }
            }
        }
        for (uint32_t i = 0; i < tb_num; i++) {
            if (next_in[i] != state[i].dirty_in) {
                state[i].dirty_in = next_in[i];
                changed = true;
            }
        }
    } while (changed);
}

static uint16_t shbr_exit_known_zero64(TranslationBlock *tb)
{
    uint16_t known_zero = 0;
    IR1_INST *ir1_list = tb->s_data->ir1;

    /* Every published TB has an independent, arbitrary dispatcher entry. */
    for (int i = 0; i < tb->icount; i++) {
        IR1_INST *ir1 = &ir1_list[i];

        if (i == tb->icount - 1 &&
            (tb->s_data->last_ir1_type == IR1_TYPE_BRANCH ||
             tb->s_data->last_ir1_type == IR1_TYPE_JUMP)) {
            break;
        }
        known_zero = shbr_known_zero_after(ir1, shbr_get_facts(ir1, 64),
                                           known_zero);
    }
    return known_zero;
}

static void shbr_finalize_zero_recovery(uint32_t tb_num,
        TranslationBlock **tb_list, uint32_t cpuid_ext_features,
        bool collect_stats, ShbrSuccessors *successors)
{
    uint32_t candidate_count[16] = { 0 };
    uint32_t restore_count[16] = { 0 };
    uint16_t candidate_mask = 0;
    uint16_t profitable = 0;

    for (uint32_t i = 0; i < tb_num; i++) {
        TranslationBlock *tb = tb_list[i];
        IR1_INST *ir1_list = tb->s_data->ir1;

        for (int j = 0; j < tb->icount; j++) {
            if (ir1_list[j].shbr_flags & LATXS_SHBR_RECOVER_ZERO64) {
                ShbrFacts facts = shbr_get_facts(&ir1_list[j], 64);

                for (uint16_t bits = facts.candidate;
                     bits; bits &= bits - 1) {
                    candidate_count[ctz32(bits)]++;
                }
                candidate_mask |= facts.candidate;
            }
        }
    }
    if (!candidate_mask) {
        shbr_clear_recover_candidates(tb_num, tb_list);
        return;
    }

    ShbrRecoverState *state = g_new0(ShbrRecoverState, tb_num);
    uint16_t *next_in = g_new0(uint16_t, tb_num);
    bool valid = true;

    shbr_compute_recover_state(tb_num, tb_list, cpuid_ext_features,
                               state, next_in, successors);

    for (uint32_t i = 0; i < tb_num; i++) {
        TranslationBlock *tb = tb_list[i];
        int ids[2];
        int nr_ids = shbr_successor_ids(tb, ids);

        for (int j = 0; j < nr_ids; j++) {
            TranslationBlock *successor_tb = tb->s_data->next_tb[ids[j]];
            int successor = successors ?
                shbr_cached_successor_index(tb_num, tb_list, successors,
                                              i, ids[j], true) :
                shbr_safe_successor_index(tb_num, tb_list, tb, ids[j],
                                           false, true);

            if (successor >= 0 && successor_tb->pc <= tb->pc) {
                for (uint16_t bits = state[i].dirty_out;
                     bits; bits &= bits - 1) {
                    restore_count[ctz32(bits)]++;
                }
            }
        }
    }
    for (int reg = 0; reg < 16; reg++) {
        if (candidate_count[reg] > restore_count[reg]) {
            profitable |= 1U << reg;
        }
    }

    /*
     * Require each recovered register to remove more hot-path writes than
     * the number of cold-edge writes it adds.  Counting per register avoids
     * accepting two independent one-for-one recovery regions merely because
     * they happen to occur in the same TU.
     */
    if (!profitable) {
        shbr_clear_recover_candidates(tb_num, tb_list);
        g_free(next_in);
        g_free(state);
        return;
    }
    for (uint32_t i = 0; i < tb_num; i++) {
        TranslationBlock *tb = tb_list[i];
        IR1_INST *ir1_list = tb->s_data->ir1;

        for (int j = 0; j < tb->icount; j++) {
            IR1_INST *ir1 = &ir1_list[j];

            if (ir1->shbr_flags & LATXS_SHBR_RECOVER_ZERO64) {
                ShbrFacts facts = shbr_get_facts(ir1, 64);

                if (!facts.candidate ||
                    (facts.candidate & profitable) != facts.candidate) {
                    ir1->shbr_flags &= ~LATXS_SHBR_RECOVER_ZERO64;
                }
            }
        }
    }

    /*
     * dirty means that the architectural high half is known zero but a
     * scalar host write was allowed to leave arbitrary physical bits there.
     * OR tracks possible dirty internal paths. It does not prove the zero
     * premise on independent dispatcher entries; check that separately at
     * every cold edge before emitting an unconditional restore.
     */
    if (profitable != candidate_mask) {
        memset(state, 0, sizeof(*state) * tb_num);
        shbr_compute_recover_state(tb_num, tb_list, cpuid_ext_features,
                                   state, next_in, successors);
    }

    for (uint32_t i = 0; i < tb_num; i++) {
        TranslationBlock *tb = tb_list[i];
        uint16_t observed = 0;
        uint16_t dirty_out = shbr_recover_transfer(
            tb, state[i].dirty_in, cpuid_ext_features, &observed);
        int ids[2];
        int nr_ids = shbr_successor_ids(tb, ids);

        if (observed) {
            valid = false;
        }
        if (!nr_ids && dirty_out) {
            valid = false;
        }
        for (int j = 0; j < nr_ids; j++) {
            int successor = successors ?
                shbr_cached_successor_index(tb_num, tb_list, successors,
                                              i, ids[j], true) :
                shbr_safe_successor_index(tb_num, tb_list, tb, ids[j],
                                           false, true);

            if (successor < 0 && dirty_out) {
                valid = false;
            }
            if (successor >= 0 && tb_list[successor]->pc <= tb->pc &&
                (dirty_out & ~shbr_exit_known_zero64(tb))) {
                valid = false;
            }
        }
    }

    if (!valid) {
        if (collect_stats) {
            SHBR_STATS_ADD(recover_invalid_tus, 1);
        }
        shbr_clear_recover_candidates(tb_num, tb_list);
        g_free(next_in);
        g_free(state);
        return;
    }

    for (uint32_t i = 0; i < tb_num; i++) {
        TranslationBlock *tb = tb_list[i];
        IR1_INST *ir1_list = tb->s_data->ir1;
        int ids[2];
        int nr_ids = shbr_successor_ids(tb, ids);

        for (int j = 0; j < tb->icount; j++) {
            IR1_INST *ir1 = &ir1_list[j];

            if (ir1->shbr_flags & LATXS_SHBR_RECOVER_ZERO64) {
                ShbrFacts facts = shbr_get_facts(ir1, 64);

                ir1->shbr_flags |= LATXS_SHBR_HIGH64;
                if (collect_stats) {
                    SHBR_STATS_ADD(known_zero_cross_tb_hits64,
                        __builtin_popcount((unsigned int)facts.candidate));
                }
            }
        }

        for (int j = 0; j < nr_ids; j++) {
            TranslationBlock *successor_tb =
                tb->s_data->next_tb[ids[j]];
            int successor = successors ?
                shbr_cached_successor_index(tb_num, tb_list, successors,
                                              i, ids[j], true) :
                shbr_safe_successor_index(tb_num, tb_list, tb, ids[j],
                                           false, true);

            if (successor >= 0 && successor_tb->pc <= tb->pc &&
                state[i].dirty_out) {
                IR1_INST *branch = &ir1_list[tb->icount - 1];

                branch->shbr_restore_zero64_exit[ids[j]] |=
                    state[i].dirty_out;
                if (collect_stats) {
                    SHBR_STATS_ADD(known_zero_cold_restores64,
                        __builtin_popcount(
                            (unsigned int)state[i].dirty_out));
                    SHBR_STATS_ADD(known_zero_recover_edges, 1);
                }
            }
        }
    }

    g_free(next_in);
    g_free(state);
}

void latxs_shbr_analyze_tu(uint32_t tb_num, TranslationBlock **tb_list,
                           uint32_t cpuid_ext_features)
{
    bool collect_stats;
    bool analyze_high32;
    bool recover_zero;
    bool has_recoverable_backedge = false;
    ShbrSuccessors *successors = NULL;

    for (uint32_t i = 0; i < tb_num; i++) {
        TranslationBlock *tb = tb_list[i];
        IR1_INST *ir1_list = tb->s_data->ir1;

        for (int j = 0; j < tb->icount; j++) {
            ir1_list[j].shbr_flags = 0;
            ir1_list[j].shbr_restore_zero64_before = 0;
            ir1_list[j].shbr_restore_zero64_after = 0;
            ir1_list[j].shbr_restore_zero64_exit[0] = 0;
            ir1_list[j].shbr_restore_zero64_exit[1] = 0;
        }
    }

    if (!latxs_shbr_enabled()) {
        return;
    }

    collect_stats = shbr_stats_enabled();
    if (collect_stats) {
        SHBR_STATS_ADD(tus, 1);
        SHBR_STATS_ADD(tbs, tb_num);
        shbr_collect_edge_stats(tb_num, tb_list);
    }

    /* These predicates do not read the IR1 marks modified by SHBR. Share
     * their results across fixed-point passes, without writing public TBs
     * or growing/clearing the OFF analysis state. */
    if (option_tu_hras && !in_aot_translate) {
        successors = g_new0(ShbrSuccessors, tb_num);
    }

    analyze_high32 = shbr_tu_may_have_candidate32(tb_num, tb_list);
    if (collect_stats) {
        SHBR_STATS_ADD(high32_tus, analyze_high32);
        SHBR_STATS_ADD(high32_skipped_tus, !analyze_high32);
    }
    if (analyze_high32) {
        shbr_analyze_tu_high(tb_num, tb_list, 32, cpuid_ext_features,
                             collect_stats, false, NULL, successors);
    }
    shbr_analyze_tu_high(tb_num, tb_list, 64, cpuid_ext_features,
                         collect_stats, false,
                         &has_recoverable_backedge, successors);
    recover_zero = !in_aot_translate && !option_aot_save &&
                   has_recoverable_backedge &&
                   shbr_tu_has_zero_def64(tb_num, tb_list, true);
    if (recover_zero) {
        shbr_analyze_tu_high(tb_num, tb_list, 64, cpuid_ext_features,
                             collect_stats, true, NULL, successors);
    }
    shbr_analyze_known_zero_tu(tb_num, tb_list, cpuid_ext_features,
                               collect_stats);
    if (recover_zero) {
        shbr_finalize_zero_recovery(tb_num, tb_list, cpuid_ext_features,
                                    collect_stats, successors);
    }
    if (successors) {
        g_free(successors);
    }
}

#endif
