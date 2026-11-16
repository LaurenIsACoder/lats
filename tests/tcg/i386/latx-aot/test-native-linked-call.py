#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify direct native call linking format checks, cold resolver, HAMT poison, and open-link closure."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
fmt_src = (root / "target/i386/latx/sbt/sys-aot-format.c").read_text()
load_src = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
hamt_src = (root / "target/i386/hamt/hamt.c").read_text()
tr_src = (root / "target/i386/latx/translator/translate.c").read_text()
header_src = (root / "target/i386/latx/include/translate.h").read_text()

frame_defs = re.search(
    r"#ifdef CONFIG_LATX_OPT_NOFP\n#define LATXS_SCS_MMX_FRAME_SIZE.*?#endif",
    header_src,
    re.S,
)[0]


def extract_between(src: str, begin: str, end: str) -> str:
    i = src.index(begin)
    j = src.index(end, i) + len(end)
    return src[i:j]


def extract_fn(src: str, name: str) -> str:
    start = src.rfind("\n", 0, src.index(name + "(")) + 1
    brace = src.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (src[end] == "{") - (src[end] == "}")
        end += 1
    return src[start:end]


# ---------------------------------------------------------------------------
# 1. Format validator test (sys-aot-format.c)
# ---------------------------------------------------------------------------
fmt_chunk = extract_between(
    fmt_src,
    "static bool aot_reloc_slots_valid(const aot_rel *rel)",
    "    return aot_native_complete_layout_valid(tu, blocks, original_end);\n}",
)

fmt_program = (
    r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_LATX_TU 1
#define TARGET_PAGE_SIZE 4096ULL
#define TARGET_PAGE_MASK (~UINT64_C(4095))
#define TB_JMP_OFFSET_INVALID 0xffff
#define LATX_RAS_TP_SIZE 512
#define LATX_RAS_TP_CALL_COLD_INSNS 5
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define g_autofree __attribute__((cleanup(free_ptr)))
static void free_ptr(void *p) { free(*(void **)p); }
#define g_try_new(type, n) ((type *)calloc((n), sizeof(type)))

static inline int64_t sextract64(uint64_t value, int start, int length)
{
    return ((int64_t)(value << (64 - length - start))) >> (64 - length);
}

typedef struct {
    uint64_t pad[2];
    struct { uint64_t stack[2]; } ras_tp;
    uint64_t eip;
    uint64_t tu_generation_array;
    uintptr_t latx_njc_cache_ptr;
    uint32_t hflags;
} CPUX86State;

#include "lats/aot-format.h"
#include "lats/aot-atomic.h"
#include "lats/aot-cpu-query.h"
#include "lats/aot-recover.h"
#include "lats/aot-system.h"
#include "lats/aot-x87.h"
"""
    + fmt_chunk
    + "\n#endif\n"
    + r"""
static uint32_t encode_b(uint32_t op, int64_t from, int64_t to)
{
    int64_t off = (to - from) >> 2;
    return op | ((off >> 16) & 0x3ffU) | (((uint32_t)off & 0xffffU) << 10);
}

static void build_valid_linked_tu(uint8_t *buf, bool leaf)
{
    memset(buf, 0, 4096);
    aot_tu_head *tu = (void *)buf;
    aot_tu_page *pages = (void *)(tu + 1);
    tu->tb_magic = AOT_TB_MAGIC;
    tu->tb_count = 1;
    tu->first_pc = 0x401000;
    tu->page_count = 1;
    pages[0].pc_delta = 0;

    aot_block *blocks = (void *)(pages + 1);
    blocks[0].pc = 0x401000;
    blocks[0].size = 16;
    blocks[0].icount = 2;
    blocks[0].ir2_num = 2;
    blocks[0].tc.offset_in_tu = 0;
    blocks[0].jmp_insn_offset[0] = leaf ? TB_JMP_OFFSET_INVALID : 4;
    blocks[0].jmp_insn_offset[1] = TB_JMP_OFFSET_INVALID;

    uint32_t *code = (void *)(blocks + 1);
    /* Ordinary TB code [0..8) */
    code[0] = 0x03400000U;
    code[1] = 0x03400000U;

    /* Native body starts at byte 8 (8-byte aligned): words [2..42) */
    uint32_t *body = code + 2;
    size_t ras_off = offsetof(CPUX86State, ras_tp.stack);
    /* F_compat [0..7] (32 bytes, fallthrough into F_direct) */
    body[0] = 0x002ddc4bU;
    body[1] = 0x26000164U | (((ras_off - 16) >> 2) << 10);
    body[2] = 0x26000161U | (((ras_off - 8) >> 2) << 10);
    body[3] = 0x00128814U;
    body[4] = 0x0011d042U;
    body[5] = 0x03400000U;
    body[6] = 0x03400000U;
    body[7] = 0x03400000U;

    /* F_direct starts at body + 8 (offset 32 within body = byte 40 in TU) */
    if (!leaf) {
        /* body[8] = call slot (TU byte 40), body[9] = continuation (TU byte 44) */
        /* body[14..15] = poison (TU bytes 64..72); body[16..23] = cold stub (TU bytes 72..104) */
        body[8] = encode_b(0x54000000U, 40, 72);
        body[9] = 0x03240063U; /* lu52i.d sp, sp, 0x900 */
        body[10] = 0x28c00385U; /* ld.d a1, guest_rsp, 0 */
        body[11] = 0x02c0239cU; /* addi.d guest_rsp, guest_rsp, 8 */
        body[12] = 0x5c000885U; /* bne a0, a1, +8 -> poison at body[14] */
        body[13] = 0x4c000020U; /* jirl zero, ra, 0 */
        body[14] = 0x1e000004U;
        body[15] = 0x4c000080U;
        body[16] = 0x14000005U;
        body[17] = 0x038000a5U;
        body[18] = 0x16000005U;
        body[19] = 0x29c002e5U | (offsetof(CPUX86State, eip) << 10);
        body[20] = 0x1400000aU;
        body[21] = 0x0380014aU;
        body[22] = 0x1600000aU;
        body[23] = 0x50000000U;
    } else {
        body[8] = 0x28c00385U;
        body[9] = 0x02c0239cU;
        body[10] = 0x5c000885U;
        body[11] = 0x4c000020U;
        body[12] = 0x1e000004U;
        body[13] = 0x4c000080U;
    }

    uint32_t code_words = leaf ? 14 : 24;
    uint32_t code_bytes = code_words * 4;
    NativeRecoverHeader hdr = {0};
    hdr.magic = NATIVE_RECOVER_MAGIC;
    hdr.bytes = sizeof(hdr);
    hdr.code_bytes = code_bytes;
    hdr.points = 0;
    hdr.state_bytes = 0;
    hdr.frame_bytes = leaf ? 0 : 32;
    NativeRecoverFooter ftr = {
        .magic = NATIVE_RECOVER_FOOTER,
        .offset = code_bytes,
        .bytes = sizeof(hdr),
    };
    uint32_t recover_bytes = sizeof(hdr) + sizeof(ftr);
    memcpy((uint8_t *)body + code_bytes, &hdr, sizeof(hdr));
    memcpy((uint8_t *)body + code_bytes + sizeof(hdr), &ftr, sizeof(ftr));

    uint32_t total_native_slots = code_words + recover_bytes / 4;
    uint32_t tu_code_size = 8 + total_native_slots * 4;
    blocks[0].tu_size = tu_code_size;

    aot_rel *rels = (void *)((uint8_t *)code + tu_code_size);
    if (!leaf) {
        tu->rel_count = 5;
        blocks[0].rel_start = 0;
        blocks[0].rel_end = 4;
        rels[0] = (aot_rel){AOT_NATIVE_CORE, 8, total_native_slots, 16, 32};
        rels[1] = (aot_rel){LOAD_GUEST_ADDR, 72, 3, 16 + 32, -1};
        rels[2] = (aot_rel){LOAD_TB_FULL_ADDR, 88, 3, 0, -1};
        rels[3] = (aot_rel){CS_NATIVE_TO_BT_RET_N0, 100, 1, 0, -1};
        rels[4] = (aot_rel){AOT_NATIVE_LINKED_CALL, 40, 1,
                            ((uint64_t)32U << 32) | 72U, 64};
    } else {
        tu->rel_count = 2;
        blocks[0].rel_start = 0;
        blocks[0].rel_end = 1;
        rels[0] = (aot_rel){AOT_NATIVE_CORE, 8, total_native_slots, 16, 32};
        rels[1] = (aot_rel){AOT_NATIVE_RET_MISS, 8 + 12 * 4, 2, 0, -1};
    }
}

int main(void)
{
    aot_rel slot_rel = { .kind = AOT_NATIVE_LINKED_CALL, .rel_slots_num = 1 };
    assert(aot_reloc_slots_valid(&slot_rel));
    slot_rel.rel_slots_num = 2;
    assert(!aot_reloc_slots_valid(&slot_rel));

    uint8_t buf[4096];
    /* Valid non-leaf linked call + dual-entry core */
    build_valid_linked_tu(buf, false);
    aot_tu_head *tu = (void *)buf;
    aot_block *blocks = (void *)((aot_tu_page *)(tu + 1) + 1);
    assert(aot_native_layout_valid(tu, blocks));

    /* Valid leaf dual-entry core with AOT_NATIVE_RET_MISS */
    build_valid_linked_tu(buf, true);
    assert(aot_native_layout_valid(tu, blocks));

    /* Reject corrupted F_compat word */
    build_valid_linked_tu(buf, false);
    uint32_t *body = (uint32_t *)(blocks + 1) + 2;
    body[3] ^= 4U;
    assert(!aot_native_layout_valid(tu, blocks));

    /* Reject zero-frame AOT_NATIVE_LINKED_CALL */
    build_valid_linked_tu(buf, false);
    NativeRecoverHeader *hdr = (void *)((uint8_t *)body + 24 * 4);
    hdr->frame_bytes = 0;
    assert(!aot_native_layout_valid(tu, blocks));

    /* Reject missing lu52i.d sp, sp, 0x900 at slot + 4 */
    build_valid_linked_tu(buf, false);
    body[9] = 0x03400000U;
    assert(!aot_native_layout_valid(tu, blocks));

    /* Reject unexpected BL inside F_compat */
    build_valid_linked_tu(buf, false);
    body[5] = encode_b(0x54000000U, 8 + 5 * 4, 40);
    assert(!aot_native_layout_valid(tu, blocks));

    /* Reject internal branch targeting continuation (slot + 4) */
    build_valid_linked_tu(buf, false);
    body[10] = encode_b(0x50000000U, 8 + 10 * 4, 44);
    assert(!aot_native_layout_valid(tu, blocks));

    /* OSR header & TU member validation */
    {
        uint8_t rec_buf[sizeof(NativeRecoverHeader) + sizeof(NativeRecoverFooter) + 16];
        memset(rec_buf, 0, sizeof(rec_buf));
        NativeRecoverHeader *rh = (void *)(rec_buf + 16);
        NativeRecoverFooter *rf = (void *)(rh + 1);
        rh->magic = NATIVE_RECOVER_MAGIC;
        rh->bytes = sizeof(*rh);
        rh->code_bytes = 16;
        rh->frame_bytes = 32;
        rf->magic = NATIVE_RECOVER_FOOTER;
        rf->offset = 16;
        rf->bytes = sizeof(*rh);
        bool rec = false;

        rh->osr_count = 2;
        rh->osr_guest_offsets[0] = 0;
        rh->osr_guest_offsets[1] = 32;
        assert(native_recover_parse(rec_buf, sizeof(rec_buf), &rec) == rh && rec);

        /* Reject osr_count > 16 */
        rh->osr_count = 17;
        assert(native_recover_parse(rec_buf, sizeof(rec_buf), &rec) == NULL);
        rh->osr_count = 2;

        /* Reject duplicate non-zero osr_guest_offsets */
        rh->osr_guest_offsets[0] = 32;
        assert(native_recover_parse(rec_buf, sizeof(rec_buf), &rec) == NULL);

        /* Reject all-zero osr_guest_offsets when osr_count > 0 */
        rh->osr_guest_offsets[0] = 0;
        rh->osr_guest_offsets[1] = 0;
        assert(native_recover_parse(rec_buf, sizeof(rec_buf), &rec) == NULL);

        /* Reject non-zero trailing slot >= osr_count */
        rh->osr_guest_offsets[1] = 32;
        rh->osr_guest_offsets[2] = 64;
        assert(native_recover_parse(rec_buf, sizeof(rec_buf), &rec) == NULL);
        rh->osr_guest_offsets[2] = 0;

        /* Reject osr_count && points */
        rh->points = 1;
        assert(native_recover_parse(rec_buf, sizeof(rec_buf), &rec) == NULL);

        /* Reject OSR offset without matching non-core TU member in layout check */
        build_valid_linked_tu(buf, true);
        NativeRecoverHeader *lhdr = (void *)((uint8_t *)body + 14 * 4);
        lhdr->osr_count = 1;
        lhdr->osr_guest_offsets[0] = 32;
        assert(!aot_native_layout_valid(tu, blocks));
    }

    puts("PASS: sys-aot-format linked call & dual-entry validation");
    return 0;
}
"""
)

# ---------------------------------------------------------------------------
# 2. Cold resolver & replaceable entry check (sys-aot-load.c)
# ---------------------------------------------------------------------------
load_program = (
    r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HF_CS64_MASK (1U << 15)
#define HF_CPL_MASK 3U
#define AOT_LINKED_COLD_BYTES (48 * sizeof(uint32_t))
#define GET_SC_TABLE(cpu, name) ((void)(cpu), sc_table.name)

static struct {
    uintptr_t scs_prologue;
    uintptr_t scs_epilogue;
    uintptr_t cs_native_to_bt_ret_0;
} sc_table;

typedef struct { unsigned reset_pending; } LatxRasTPState;
typedef struct {
    uint64_t eip;
    LatxRasTPState ras_tp;
} CPUArchState;
typedef struct {
    uint32_t flags;
    uint64_t cs_base;
} TranslationBlock;

static bool scs_enabled(void) { return true; }
static uintptr_t latx_helper_aot_linked_call(void) { return 0; }
static int64_t sextract64(uint64_t value, unsigned start, unsigned length)
{
    return (int64_t)(value << (64 - start - length)) >> (64 - length);
}
static void flush_idcache_range(uintptr_t a, uintptr_t b, unsigned n)
{
    (void)a; (void)b; (void)n;
}
"""
    + frame_defs
    + "\n"
    + extract_fn(load_src, "aot_native_entry_replaceable")
    + "\n"
    + extract_fn(load_src, "aot_call_load64")
    + "\n"
    + extract_fn(load_src, "aot_call_far")
    + "\n"
    + extract_fn(load_src, "aot_native_linked_call_resolver")
    + r"""
int main(void)
{
    uint32_t body[24];
    for (unsigned i = 0; i < 24; i++) body[i] = 0x03400000U;
    assert(aot_native_entry_replaceable(body, sizeof(body), 32));
    /* Branch targeting byte 32 (from byte 40, delta = -2) must be rejected! */
    body[10] = 0x50000000U | (((uint32_t)(-2) & 0xffffU) << 10) | 0x3ffU;
    assert(!aot_native_entry_replaceable(body, sizeof(body), 32));
    body[10] = 0x03400000U;
    /* Branch targeting byte 0 must be rejected! */
    body[10] = 0x50000000U | (((uint32_t)(-10) & 0xffffU) << 10) | 0x3ffU;
    assert(!aot_native_entry_replaceable(body, sizeof(body), 32));

    static uint32_t arena[512];
    uint32_t *slot = arena + 32;
    uint32_t *stub = arena + 64;
    sc_table.scs_prologue = (uintptr_t)(arena + 256);
    sc_table.scs_epilogue = (uintptr_t)(arena + 288);
    sc_table.cs_native_to_bt_ret_0 = (uintptr_t)(arena + 320);
    TranslationBlock tb = { .flags = HF_CS64_MASK | HF_CPL_MASK, .cs_base = 0 };
    *slot = 0x54001000U;
    assert(aot_native_linked_call_resolver(stub, slot, &tb, 0x402000, 0));
    int64_t patched_off = sextract64(((*slot & 0x3ffU) << 16) | ((*slot >> 10) & 0xffffU), 0, 26);
    assert((*slot & 0xfc000000U) == 0x54000000U);
    assert((intptr_t)slot + patched_off * 4 == (intptr_t)stub);
    assert(stub[35] == 0x032400c6U); /* lu52i.d a2, a2, 0x900 */
    assert(stub[39] == 0x4c0000c0U); /* jirl zero, a2, 0 */
    puts("PASS: sys-aot-load replaceable check & linked call cold resolver");
    return 0;
}
"""
)

# ---------------------------------------------------------------------------
# 3. HAMT invalidation poison check (hamt.c)
# ---------------------------------------------------------------------------
hamt_body = extract_fn(hamt_src, "void hamt_poison_tb_entry")
hamt_program = (
    r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define CONFIG_LATX_TU 1
#define HF_CS64_MASK 1U
#define HF_CPL_MASK 6U
#define CF_PCREL 8U
#define CF_NO_ACTIVE 16U
#define IS_AOT_TB 32U
#define IS_TU_TB 64U
#define HAMT_INVALID_TB_INSN 1U
#define hamt_enabled() true
#define qatomic_read(p) (*(p))
#define qatomic_set(p, v) (*(p) = (v))
#define qatomic_load_acquire(p) (*(p))
#define TB_TU_PAGES(tb) ((tb)->pages)
#define CPU_FOREACH(cpu) for ((cpu) = NULL; (cpu) != NULL; )

struct TranslationBlock;
typedef struct {
    const void *aot_native_call_entry;
    const void *aot_direct_native_entry;
} SeparatedData;

typedef struct {
    struct TranslationBlock *tb;
    uint32_t *core;
    uint32_t core_invalid_insn;
    uint32_t *direct_native_entry;
    uint32_t direct_entry_invalid_insn;
    uint32_t *direct_continuation;
    uint32_t direct_continuation_invalid;
    uint32_t osr_count;
    struct TranslationBlock *osr_tbs[16];
    uint32_t *osr_entries[16];
} AOTNativeRecord;

typedef struct {
    uint32_t count;
    AOTNativeRecord records[1];
} AOTNativeTUInfo;

typedef struct {
    AOTNativeTUInfo *native;
} TranslationUnitPages;

typedef struct TranslationBlock {
    struct { void *ptr; } tc;
    uint64_t pc;
    unsigned ir2_num, flags, cflags, bool_flags, cs_base;
    SeparatedData *s_data;
    TranslationUnitPages *pages;
} TranslationBlock;

typedef struct CPUState { int dummy; } CPUState;

static unsigned tb_cflags(const TranslationBlock *tb) { return tb->cflags; }
static bool tb_is_tu(const TranslationBlock *tb) { return (tb->bool_flags & IS_TU_TB) != 0; }
static void tb_jmp_cache_invalidate(CPUState *cpu, uint64_t pc, TranslationBlock *tb)
{ (void)cpu; (void)pc; (void)tb; }
static unsigned flushes;
static void flush_idcache_range(uintptr_t a, uintptr_t b, unsigned size)
{ assert(a == b && size == 4); flushes++; }
"""
    + hamt_body
    + r"""
int main(void)
{
    uint32_t tb_entry = 0x11111111U;
    uint32_t body[32];
    for (unsigned i = 0; i < 32; i++) body[i] = 0x03400000U;
    SeparatedData sdata = {
        .aot_native_call_entry = body,
        .aot_direct_native_entry = body + 8,
    };
    AOTNativeTUInfo info = {
        .count = 1,
        .records = {{
            .core = body,
            .core_invalid_insn = 0x50000400U,
            .direct_native_entry = body + 8,
            .direct_entry_invalid_insn = 0x50000800U,
            .direct_continuation = body + 12,
            .direct_continuation_invalid = 0x50000c00U,
        }},
    };
    TranslationUnitPages pages = { .native = &info };
    TranslationBlock tb = {
        .tc = { &tb_entry },
        .ir2_num = 4,
        .flags = HF_CS64_MASK | HF_CPL_MASK,
        .bool_flags = IS_AOT_TB | IS_TU_TB,
        .s_data = &sdata,
        .pages = &pages,
    };
    uint32_t osr_stub[10] = { 0x03800004U, 0x033fc084U };
    SeparatedData osr_sdata = {
        .aot_native_call_entry = NULL,
        .aot_direct_native_entry = osr_stub,
    };
    TranslationBlock osr_tb = {
        .pc = 0x401080ULL,
        .s_data = &osr_sdata,
    };
    info.records[0].tb = &tb;
    info.records[0].osr_count = 1;
    info.records[0].osr_tbs[0] = &osr_tb;
    info.records[0].osr_entries[0] = osr_stub;

    hamt_poison_tb_entry(&tb);
    assert(tb_entry == HAMT_INVALID_TB_INSN);
    assert(body[0] == 0x50000400U);       /* +0x00 F_compat entry poisoned */
    assert(body[8] == 0x50000800U);       /* +0x20 F_direct entry poisoned */
    assert(body[12] == 0x50000c00U);      /* slot + 4 continuation poisoned */
    assert(osr_stub[0] == 0x50001000U);   /* OSR stub branches to +16 fallback */
    assert(sdata.aot_native_call_entry == NULL);
    assert(sdata.aot_direct_native_entry == NULL);
    assert(osr_sdata.aot_direct_native_entry == NULL);
    assert(flushes == 5);
    puts("PASS: hamt_poison_tb_entry poisons compat, direct, OSR, and continuation");
    return 0;
}
"""
)

# ---------------------------------------------------------------------------
# 4. Linking & open-call invalidation test (translate.c)
# ---------------------------------------------------------------------------
tr_chunk = extract_between(
    tr_src,
    "static bool aot_native_branch_fits(intptr_t from, intptr_t to)",
    "void latxs_aot_close_open_linked_calls(CPUArchState *env)",
)
tr_close_fn = extract_fn(tr_src, "void latxs_aot_close_open_linked_calls")
tr_target_chunk = extract_between(
    tr_src,
    "static bool aot_native_direct_call_target(",
    "const void *latxs_aot_linked_call_replay(TranslationBlock *source,\n                                         TranslationBlock *target)",
)
tr_replay_fn = extract_fn(tr_src, "const void *latxs_aot_linked_call_replay")

tr_program = (
    r"""
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LATX_TU 1
#define CONFIG_SOFTMMU 1
#define IS_AOT_TB 1U
#define IS_TU_TB 2U
#define CF_INVALID (1U << 18)
#define CF_NO_ACTIVE (1U << 19)
#define CF_SINGLE_STEP (1U << 20)
#define CF_USE_ICOUNT (1U << 21)
#define HF_CS64_MASK (1U << 15)
#define HF_CPL_MASK 3U
#define IRQ_CHECK_ASYNC_NOTIFY 3
#define MF_MTCC_ID_MASK 1U
#define MF_MTCC_ID_SHIFT 0
#define DMW_OUT(x) ((uintptr_t)(x))
#define qatomic_read(p) (*(p))
#define qatomic_set(p, v) (*(p) = (v))
#define qatomic_load_acquire(p) (*(p))
#define g_assert(c) assert(c)
#define g_renew(type, mem, n) ((type *)realloc((mem), (n) * sizeof(type)))
#define qemu_spin_lock(l) ((void)(l))
#define qemu_spin_unlock(l) ((void)(l))
#define qemu_thread_jit_write() ((void)0)
#define flush_idcache_range(a, b, c) ((void)(a), (void)(b), (void)(c))

static bool option_soft_ras = true;
static bool option_rastoptp = true;
static int option_int_opt = IRQ_CHECK_ASYNC_NOTIFY;
static uint64_t option_tucp_generation_array_size = 64;

static inline int64_t sextract64(uint64_t value, int start, int length)
{
    return ((int64_t)(value << (64 - length - start))) >> (64 - length);
}

typedef struct {
    uint32_t state_bytes;
} NativeRecoverHeader;

struct TranslationBlock;
typedef struct {
    struct TranslationBlock *tb;
    void *entry;
    uint32_t *core;
    const NativeRecoverHeader *recover;
    uint64_t callee_pc;
    uint32_t *direct_call_slot;
    uint32_t direct_call_closed;
    uint32_t *direct_native_entry;
    bool direct_call_guards_emitted;
    bool direct_call_linked;
    bool linked_call_open;
} AOTNativeRecord;

typedef struct {
    uint32_t count;
    AOTNativeRecord records[2];
} AOTNativeTUInfo;

typedef struct {
    uint32_t id;
    bool covered_by_caller;
    AOTNativeTUInfo *native;
} TranslationUnitPages;

typedef struct {
    const void *aot_native_call_entry;
    const void *aot_direct_native_entry;
} SeparatedData;

typedef struct TranslationBlock {
    struct { void *ptr; } tc;
    uint64_t pc;
    uint64_t cs_base;
    uint32_t flags;
    uint32_t cflags;
    uint32_t bool_flags;
    uint32_t misc_flags;
    uintptr_t jmp_dest[2];
    int jmp_lock;
    SeparatedData *s_data;
    TranslationUnitPages *pages;
} TranslationBlock;

typedef struct {
    uintptr_t tu_generation_array;
    uint16_t tu_generation;
    void **aot_open_linked_calls;
    uint32_t aot_open_linked_calls_count;
    uint32_t aot_open_linked_calls_cap;
} CPUX86State;
typedef CPUX86State CPUArchState;

typedef struct {
    CPUX86State *env_ptr;
} CPUState;

static CPUState *current_cpu;

#define TB_TU_PAGES(tb) ((tb)->pages)
static bool tb_is_tu(const TranslationBlock *tb) { return (tb->bool_flags & IS_TU_TB) != 0; }
static uint32_t tb_cflags(const TranslationBlock *tb) { return tb->cflags; }
static uint32_t get_mask_from_miscflags(const TranslationBlock *tb, uint32_t m, uint32_t s)
{ return (tb->misc_flags & m) >> s; }
static bool tucp_generation_enabled(void) { return true; }
static bool tucp_generation_counter(void) { return false; }
static uint64_t tucp_generation_entry_size(void) { return 1; }
static bool tucp_generation_offset(void) { return true; }
static bool tucp_fast_direct_check(void) { return true; }
static bool can_do_hamt(CPUX86State *env) { (void)env; return true; }
static bool hamt_mode_dmw(void) { return true; }
static bool latxs_tucp_pages_cover(const TranslationUnitPages *caller,
                                   const TranslationUnitPages *callee)
{
    (void)caller;
    return callee->covered_by_caller;
}
static const void *latxs_aot_direct_call_entry(TranslationBlock *source, int n,
                                               TranslationBlock *target)
{
    (void)source; (void)n;
    return target->s_data->aot_native_call_entry;
}
"""
    + tr_chunk[: tr_chunk.rindex("void latxs_aot_close_open_linked_calls")]
    + tr_close_fn
    + "\n"
    + tr_target_chunk[: tr_target_chunk.rindex("const void *latxs_aot_linked_call_replay")]
    + tr_replay_fn
    + r"""
int main(void)
{
    uint8_t gen_array[64] = {0};
    CPUX86State env = { .tu_generation_array = (uintptr_t)gen_array };
    CPUState cpu = { .env_ptr = &env };
    current_cpu = &cpu;

    uint32_t code[64];
    for (unsigned i = 0; i < 64; i++) code[i] = 0x03400000U;
    uint32_t *covered_slot = &code[4];
    uint32_t *cross_slot = &code[8];
    uint32_t *callee_compat = &code[16];
    uint32_t *callee_direct = &code[32];
    *covered_slot = 0x54000004U;
    *cross_slot = 0x54000008U;

    NativeRecoverHeader rec_hdr = { .state_bytes = 0 };

    SeparatedData caller_sdata = {0};
    AOTNativeTUInfo caller_info = {
        .count = 2,
        .records = {
            {
                .callee_pc = 0x402000,
                .direct_call_slot = covered_slot,
                .direct_call_closed = 0x54000004U,
                .direct_call_linked = true,
            },
            {
                .callee_pc = 0x403000,
                .direct_call_slot = cross_slot,
                .direct_call_closed = 0x54000008U,
                .direct_call_linked = true,
            },
        },
    };
    TranslationUnitPages caller_pages = { .id = 1, .native = &caller_info };
    TranslationBlock caller_cov_tb = {
        .tc = { &code[0] }, .pc = 0x401000,
        .flags = HF_CS64_MASK | HF_CPL_MASK, .bool_flags = IS_AOT_TB | IS_TU_TB,
        .s_data = &caller_sdata, .pages = &caller_pages,
    };
    TranslationBlock caller_x_tb = caller_cov_tb;
    caller_x_tb.pc = 0x401100;
    caller_info.records[0].tb = &caller_cov_tb;
    caller_info.records[1].tb = &caller_x_tb;

    /* Covered callee TU */
    SeparatedData cov_sdata = {
        .aot_native_call_entry = callee_compat,
        .aot_direct_native_entry = callee_direct,
    };
    AOTNativeTUInfo cov_info = {
        .count = 1,
        .records = {{
            .entry = callee_compat, .core = callee_compat,
            .direct_native_entry = callee_direct, .recover = &rec_hdr,
        }},
    };
    TranslationUnitPages cov_pages = { .id = 2, .covered_by_caller = true, .native = &cov_info };
    TranslationBlock cov_tb = {
        .tc = { callee_compat }, .pc = 0x402000,
        .flags = HF_CS64_MASK | HF_CPL_MASK, .bool_flags = IS_AOT_TB | IS_TU_TB,
        .s_data = &cov_sdata, .pages = &cov_pages,
    };
    cov_info.records[0].tb = &cov_tb;
    caller_cov_tb.jmp_dest[0] = (uintptr_t)&cov_tb;

    /* Cross-TU callee */
    SeparatedData x_sdata = cov_sdata;
    AOTNativeTUInfo x_info = cov_info;
    TranslationUnitPages x_pages = { .id = 3, .covered_by_caller = false, .native = &x_info };
    TranslationBlock x_tb = cov_tb;
    x_tb.pc = 0x403000;
    x_tb.s_data = &x_sdata;
    x_tb.pages = &x_pages;
    x_info.records[0].tb = &x_tb;
    caller_x_tb.jmp_dest[0] = (uintptr_t)&x_tb;

    /* 1. Covered callee links without needing gen_array[2] and is NOT tracked in open list */
    assert(latxs_aot_linked_call_replay(&caller_cov_tb, &cov_tb) == callee_direct);
    assert(*covered_slot == aot_native_branch(0x54000000U, (intptr_t)covered_slot, (intptr_t)callee_direct));
    assert(env.aot_open_linked_calls_count == 0);

    /* 2. Cross-TU callee fails when gen_array[3] == 0 (not cached valid!) */
    assert(latxs_aot_linked_call_replay(&caller_x_tb, &x_tb) == NULL);
    assert(*cross_slot == 0x54000008U);
    assert(env.aot_open_linked_calls_count == 0);

    /* 3. Cross-TU callee succeeds once gen_array[3] == 1 and records in open list */
    gen_array[3] = 1;
    assert(latxs_aot_linked_call_replay(&caller_x_tb, &x_tb) == callee_direct);
    assert(*cross_slot == aot_native_branch(0x54000000U, (intptr_t)cross_slot, (intptr_t)callee_direct));
    assert(env.aot_open_linked_calls_count == 1);
    assert(caller_info.records[1].linked_call_open);

    /* 4. Closing open linked calls resets cross_slot to closed, keeps covered_slot open! */
    latxs_aot_close_open_linked_calls(&env);
    assert(env.aot_open_linked_calls_count == 0);
    assert(!caller_info.records[1].linked_call_open);
    assert(*cross_slot == 0x54000008U);
    assert(*covered_slot == aot_native_branch(0x54000000U, (intptr_t)covered_slot, (intptr_t)callee_direct));

    free(env.aot_open_linked_calls);
    puts("PASS: translate.c covered vs cross-TU linked call patch & open-link closure");
    return 0;
}
"""
)

with tempfile.TemporaryDirectory(prefix="lats-native-linked-call-") as tmp:
    d = Path(tmp)
    # 1. Format test
    (d / "fmt.c").write_text(fmt_program)
    subprocess.run(
        [
            "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
            "-DLATS_AOT_DISK_TU", "-I", str(root / "include"),
            str(d / "fmt.c"), "-o", str(d / "fmt"),
        ],
        check=True,
    )
    subprocess.run([str(d / "fmt")], check=True)

    # 2. Load/resolver test (both FP and NOFP modes)
    (d / "load.c").write_text(load_program)
    for nofp in (False, True):
        subprocess.run(
            [
                "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                *(["-DCONFIG_LATX_OPT_NOFP"] if nofp else []),
                str(d / "load.c"), "-o", str(d / "load"),
            ],
            check=True,
        )
        subprocess.run([str(d / "load")], check=True)

    # 3. HAMT test
    (d / "hamt.c").write_text(hamt_program)
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(d / "hamt.c"), "-o", str(d / "hamt")],
        check=True,
    )
    subprocess.run([str(d / "hamt")], check=True)

    # 4. Translate linking/closing test
    (d / "tr.c").write_text(tr_program)
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(d / "tr.c"), "-o", str(d / "tr")],
        check=True,
    )
    subprocess.run([str(d / "tr")], check=True)

print("PASS: all direct native call linking unit tests succeeded")
