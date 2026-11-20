/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sys-aot.h"
#include "sys-aot-internal.h"
#include "sys-aot-format.h"
#include "latx-options.h"
#include "qemu/bitops.h"
#include "qemu/xxhash.h"

/* cross_page */
/* instptn */
/* aes_opt */
/* opt_mmio */
/* tb_max_insns */
#define PAGE_PCREL_BIT 0
#define TB_PCREL_BIT 1
#define STATICCS_BIT 2
#define TB_LINK_BIT 3
#define INTB_LINK_BIT 4
#define INTB_NJC_BIT 5
#define SOFT_RAS_BIT 8
#define PAUSE_BIT 9
#define FAST_HTB_LOOKUP_BIT 11
#define FLAGED_SHT_BIT 12
#define FAST_ITLB_BIT 13
#define LAZYPC_BIT 14
#define LINK_OPT_BIT 15
#define LSFPU_BIT 16
#define FAST_FPR_LDST_BIT 17
#define FLAG_REDUCTION_BIT 18
#define RISK_MEM_OFFSET_BIT 19
#define RISK_SIMD_IGNORE_UPPER64_BIT 20
#define RISK_SIMD_IGNORE_UPPER64_STAT_TRACK_BIT 21
#define LAZY_MXCSR_BIT 22
#define RISK_IN_379_BIT 23
#define FAST_ATOMIC_BIT 24
#define SIGFPE_BIT 25
#define FAST_MASKED_V_BIT 26
#define SOFT_FPU_BIT 27
#define SOFT_FPU_FAST_BIT 28
#define NO_RESERVE_256_BIT 29
#define HAMT_BIT 30
#define COMMON_SLOWPATH_BIT 31
#define LARGE_CODE_CACHE_BIT 32
#define INT_OPT_BIT 33
#define INHIBIT_IRQ_BIT 34
#define REGION_PROLOGUE_BIT 35
#define SPLIT_TB_BIT 36
#define SPLIT_SEARCH_BIT 37
#define OPT_SYSCALL_BIT 38
#define OPT_SYSRET_BIT 39
#define OPT_IRET_BIT 40
#define OPT_RDTSC_RDTSCP_BIT 41
#define SMMU_SLOW_BIT 42
#define HAMT_MODE_BIT 43
#define HAMT_SUBMODE_BIT 44
#define HAMT_CONFIG_BIT 46 /* 45~46 */
#define PUSHPOP 47
#define FAST_ITLB_PTW_BIT 48
#define TU_MAX_TB_SHIFT 49
#define TU_MAX_TB_MASK UINT64_C(0x1ff)
#define AOT_GPR_HIGHBITS_BIT 58
#define INTB_NJC_INLINE_BIT 57
#define INTB_NJC_COMPACT_BIT 59
#define INTB_NJC_FULL_BIT 60
#define INTB_NJC_LINE_BIT 62
#define AOT_TU_CROSS_PAGE_BIT 61
#define AOT_TU_CROSS_PAGE_POLICY_BIT 63

#define AOT_TU_CODEGEN_FAST_MASK 0x3f
#define AOT_TU_CODEGEN_GEN_SHIFT 6
#define AOT_TU_CODEGEN_GEN_MASK 0x3
#define AOT_TU_CODEGEN_CP_SHIFT 8
#define AOT_TU_CODEGEN_CP_MASK 0xfff
#define AOT_TB_CODEGEN_POLICY (UINT32_C(1) << 20)
#define AOT_GHBR_LEVEL_SHIFT 21
#define AOT_GHBR_LEVEL_MASK UINT32_C(0x3)
#define AOT_PCID_ASID_CODEGEN (UINT32_C(1) << 27)
#define AOT_PCID_CACHE_CODEGEN (UINT32_C(1) << 28)

/* Bits 6 and 7 were unused counter options, never emitted in the flags. */
#define SHBR_LEVEL_SHIFT 6
#define SHBR_LEVEL_MASK UINT64_C(0x3)

static int pre_file_tb_count;

typedef struct AOTLayoutTURefKey {
    uint64_t tu_id;
    int64_t anchor_delta;
} AOTLayoutTURefKey;

static guint aot_layout_tu_ref_hash(gconstpointer opaque)
{
    const AOTLayoutTURefKey *key = opaque;
    uint64_t value = key->tu_id ^ (uint64_t)key->anchor_delta;

    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    return value ^ (value >> 32);
}

static gboolean aot_layout_tu_ref_equal(gconstpointer ap, gconstpointer bp)
{
    const AOTLayoutTURefKey *a = ap;
    const AOTLayoutTURefKey *b = bp;

    return a->tu_id == b->tu_id && a->anchor_delta == b->anchor_delta;
}

static bool aot_layout_add_tu_ref(GHashTable *refs, uint64_t tu_id,
                                  int64_t anchor_delta)
{
    AOTLayoutTURefKey lookup = {
        .tu_id = tu_id,
        .anchor_delta = anchor_delta,
    };
    gpointer old_value = g_hash_table_lookup(refs, &lookup);
    guint count = GPOINTER_TO_UINT(old_value);

    if (count == G_MAXUINT) {
        return false;
    }
    g_hash_table_replace(refs, g_memdup2(&lookup, sizeof(lookup)),
                         GUINT_TO_POINTER(count + 1));
    return true;
}

static bool aot_layout_same_tu_refs(GHashTable *expected,
                                    GHashTable *actual)
{
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    if (g_hash_table_size(expected) != g_hash_table_size(actual)) {
        return false;
    }
    g_hash_table_iter_init(&iter, expected);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (g_hash_table_lookup(actual, key) != value) {
            return false;
        }
    }
    return true;
}

/*
 * validate_aot_layout() has already established every record boundary before
 * this pass. Check the cross-record invariant separately: each non-anchor TU
 * dependency has exactly one matching member-page reference.  Single-page
 * payload IDs are content identities and may legitimately repeat when equal
 * guest pages exist at different physical addresses.  Referenced cross-page
 * payload IDs, however, must remain unique in the file.
 */
static bool validate_aot_tu_references(const uint8_t *file,
                                       const page_info_array_t *info_array)
{
    GHashTable *cross_page_payloads = g_hash_table_new_full(g_int64_hash,
            g_int64_equal, g_free, NULL);
    GHashTable *expected = g_hash_table_new_full(aot_layout_tu_ref_hash,
            aot_layout_tu_ref_equal, g_free, NULL);
    GHashTable *actual = g_hash_table_new_full(aot_layout_tu_ref_hash,
            aot_layout_tu_ref_equal, g_free, NULL);
    bool ok = false;

    for (uint32_t i = 0; i < info_array->page_count; i++) {
        const page_head *page = (const page_head *)(file +
                info_array->pages[i].file_offset);
        const uint8_t *record = (const uint8_t *)page + sizeof(*page) +
                TARGET_PAGE_SIZE;

        for (uint32_t j = 0; j < page->tb_count; j++) {
            uint32_t magic = *(const uint32_t *)record;

            if (magic == AOT_TB_MAGIC) {
                const aot_tu_head *tu = (const aot_tu_head *)record;
                const aot_tu_page *pages = (const aot_tu_page *)(tu + 1);
                uint64_t lookup = tu->tu_id;

                if (tu->page_count > 1) {
                    if (g_hash_table_contains(cross_page_payloads, &lookup)) {
                        fprintf(stderr, "[AOT] Layout: duplicate cross-page "
                                "TU payload id=%" PRIx64 "\n", tu->tu_id);
                        goto out;
                    }
                    g_hash_table_insert(cross_page_payloads,
                            g_memdup2(&lookup, sizeof(lookup)),
                            GUINT_TO_POINTER(1));
                }
                for (uint32_t k = 0; k < tu->page_count; k++) {
                    if (pages[k].pc_delta &&
                            !aot_layout_add_tu_ref(expected, tu->tu_id,
                                                  -pages[k].pc_delta)) {
                        goto out;
                    }
                }
                record += tu->tu_sum_size;
            } else {
                const aot_tu_ref *ref = (const aot_tu_ref *)record;

                if (!aot_layout_add_tu_ref(actual, ref->tu_id,
                                           ref->anchor_delta)) {
                    goto out;
                }
                record += sizeof(*ref);
            }
        }
    }

    if (!aot_layout_same_tu_refs(expected, actual)) {
        fprintf(stderr, "[AOT] Layout: TU member references do not match "
                "canonical dependencies\n");
        goto out;
    }
    ok = true;

out:
    g_hash_table_destroy(actual);
    g_hash_table_destroy(expected);
    g_hash_table_destroy(cross_page_payloads);
    return ok;
}

static uint32_t get_aot_tu_codegen_flags(uint32_t aot_mode)
{
    /* Inline DMW helper returns restore either a constant or a vCPU ASID. */
    uint32_t flags = option_pcid_asid ? AOT_PCID_ASID_CODEGEN : 0;

    if (option_pcid_cache) {
        flags |= AOT_PCID_CACHE_CODEGEN;
    }
#ifdef CONFIG_LATX_TU
    if (aot_mode & AOT_USER) {
        flags |= (option_tucp_fast & AOT_TU_CODEGEN_FAST_MASK) |
            ((option_tucp_generation & AOT_TU_CODEGEN_GEN_MASK) <<
             AOT_TU_CODEGEN_GEN_SHIFT) |
            ((option_ghbr & AOT_GHBR_LEVEL_MASK) << AOT_GHBR_LEVEL_SHIFT);

        if (aot_tu_enabled()) {
            flags |= (option_cross_page & AOT_TU_CODEGEN_CP_MASK) <<
                     AOT_TU_CODEGEN_CP_SHIFT;
        } else {
            /* Legacy TB files did not identify fast/gen-dependent exits. */
            flags |= AOT_TB_CODEGEN_POLICY;
        }
        return flags;
    }
#endif
    return flags;
}

static uint64_t get_option_flags(uint32_t aot_mode)
{
    uint64_t flags = 0;
    if (option_page_pcrel) {
        flags |= 1 << PAGE_PCREL_BIT;
    }
    if (option_tb_pcrel) {
        flags |= 1 << TB_PCREL_BIT;
    }
    if (option_staticcs) {
        flags |= 1 << STATICCS_BIT;
    }
    if (option_tb_link) {
        flags |= 1 << TB_LINK_BIT;
    }
    if (option_intb_link) {
        flags |= 1 << INTB_LINK_BIT;
    }
    if (option_intb_njc) {
        flags |= 1 << INTB_NJC_BIT;
    }
    if (intb_njc_inline_enabled()) {
        flags |= UINT64_C(1) << INTB_NJC_INLINE_BIT;
        if (latx_jmp_cache_compact_enabled()) {
            flags |= UINT64_C(1) << INTB_NJC_COMPACT_BIT;
        }
        if (option_jmp_cache_hash != LATX_JMP_CACHE_HASH_PAGE) {
            if (option_jmp_cache_hash == LATX_JMP_CACHE_HASH_FULL) {
                flags |= UINT64_C(1) << INTB_NJC_FULL_BIT;
            } else {
                flags |= UINT64_C(1) << INTB_NJC_LINE_BIT;
            }
        }
    }
    if (option_soft_ras) {
        flags |= 1 << SOFT_RAS_BIT;
    }
    if (option_pause) {
        flags |= 1 << PAUSE_BIT;
    }
#ifdef CONFIG_LATX_TU
    if (aot_tu_enabled() && (aot_mode & AOT_USER)) {
        flags |= UINT64_C(1) << AOT_TU_FLAG_BIT;
        /*
         * Legacy TU files did not record aottucp: neither on nor off may
         * accept them as having a known cross-page policy.
         */
        flags |= UINT64_C(1) << AOT_TU_CROSS_PAGE_POLICY_BIT;
        if (option_aot_cross_page_tu) {
            flags |= UINT64_C(1) << AOT_TU_CROSS_PAGE_BIT;
        }
        flags |= ((uint64_t)option_tu_max_tb & TU_MAX_TB_MASK)
                 << TU_MAX_TB_SHIFT;
    }
#endif
    flags |= ((uint64_t)option_shbr & SHBR_LEVEL_MASK) << SHBR_LEVEL_SHIFT;
    if (option_flaged_sht) {
        flags |= 1 << FLAGED_SHT_BIT;
    }
    if (option_fast_itlb) {
        flags |= UINT64_C(1) << FAST_ITLB_BIT;
    }
    if (option_fast_itlb_ptw) {
        flags |= UINT64_C(1) << FAST_ITLB_PTW_BIT;
    }
    if (option_lazypc) {
        flags |= 1 << LAZYPC_BIT;
    }
    if (option_link_opt) {
        flags |= 1 << LINK_OPT_BIT;
    }
    if (option_lsfpu) {
        flags |= 1 << LSFPU_BIT;
    }
    if (option_fast_fpr_ldst) {
        flags |= 1 << FAST_FPR_LDST_BIT;
    }
    if (option_sys_flag_reduction) {
        flags |= 1 << FLAG_REDUCTION_BIT;
    }
    if (option_risk_mem_offset) {
        flags |= 1 << RISK_MEM_OFFSET_BIT;
    }
    if (option_risk_simd_ignore_upper64) {
        flags |= 1 << RISK_SIMD_IGNORE_UPPER64_BIT;
    }
    if (option_risk_simd_ignore_upper64_stat_track) {
        flags |= 1 << RISK_SIMD_IGNORE_UPPER64_STAT_TRACK_BIT;
    }
    if (option_lazy_mxcsr) {
        flags |= 1 << LAZY_MXCSR_BIT;
    }
    if (option_risk_in_379) {
        flags |= 1 << RISK_IN_379_BIT;
    }
    if (option_fast_atomic) {
        flags |= 1 << FAST_ATOMIC_BIT;
    }
    if (option_sigfpe) {
        flags |= 1 << SIGFPE_BIT;
    }
    if (option_fast_masked_v) {
        flags |= 1 << FAST_MASKED_V_BIT;
    }
    if (option_soft_fpu) {
        flags |= 1 << SOFT_FPU_BIT;
    }
    if (option_soft_fpu_fast) {
        flags |= 1 << SOFT_FPU_FAST_BIT;
    }
    if (option_no_reserve_256) {
        flags |= 1 << NO_RESERVE_256_BIT;
    }
    if (option_common_slowpath) {
        flags |= (uint64_t)1 << COMMON_SLOWPATH_BIT;
    }
    if (option_large_code_cache) {
        flags |= (uint64)1 << LARGE_CODE_CACHE_BIT;
    }
    if (option_int_opt) {
        flags |= (uint64)1 << INT_OPT_BIT;
    }
    if (option_inhibit_irq) {
        flags |= (uint64)1 << INHIBIT_IRQ_BIT;
    }
    if (option_region_prologue) {
        flags |= (uint64)1 << REGION_PROLOGUE_BIT;
    }
    if (option_split_tb) {
        flags |= (uint64)1 << SPLIT_TB_BIT;
    }
    if (option_split_search) {
        flags |= (uint64)1 << SPLIT_SEARCH_BIT;
    }
    if (option_opt_syscall) {
        flags |= (uint64)1 << OPT_SYSCALL_BIT;
    }
    if (option_opt_sysret) {
        flags |= (uint64)1 << OPT_SYSRET_BIT;
    }
    if (option_opt_iret) {
        flags |= (uint64)1 << OPT_IRET_BIT;
    }
    if (option_opt_rdtsc_rdtscp) {
        flags |= (uint64)1 << OPT_RDTSC_RDTSCP_BIT;
    }
    if (option_smmu_slow) {
        flags |= (uint64)1 << SMMU_SLOW_BIT;
    }
    if (option_hamt) {
        flags |= (uint64)1 << HAMT_BIT;
        flags |= (uint64)(option_hamt_config & 0x3) << HAMT_CONFIG_BIT;
    }
    if (option_hamt_mode) {
        flags |= (uint64)1 << HAMT_MODE_BIT;
    }
    if (option_hamt_submode) {
        flags |= (uint64)1 << HAMT_SUBMODE_BIT;
    }
    if (option_pushpop) {
        flags |= (uint64)1 << PUSHPOP;
    }
    if (option_aot_gpr_highbits && (aot_mode & AOT_USER)) {
        flags |= UINT64_C(1) << AOT_GPR_HIGHBITS_BIT;
    }
    return flags;
}

aot_tail *get_aot_tail(int fd)
{
    aot_tail *tail = (aot_tail *)malloc(sizeof(aot_tail));
    if (!tail) {
        return NULL;
    }
    if (lseek(fd, -sizeof(aot_tail), SEEK_END) == (off_t)-1
            || read(fd, tail, sizeof(aot_tail)) != sizeof(aot_tail)) {
        fprintf(stderr, "get_aot_tail fd %d fail\n", fd);
        free(tail);
        return NULL;
    }
    return tail;
}

bool save_aot_tail(int page_num, int tb_num, uint32_t aot_mode)
{
    struct aot_tail tail = { 0 };
    tail.flags = get_option_flags(aot_mode);
    tail.sys_flag_reduction_tu = option_sys_flag_reduction_tu;
    tail.njc_bits = option_intb_njc &&
        option_jmp_cache_hash != LATX_JMP_CACHE_HASH_PAGE ?
        latx_jmp_cache_bits() : 0;
    tail.aot_tu_codegen_flags = get_aot_tu_codegen_flags(aot_mode);
    tail.page_num = page_num;
    tail.tb_num = tb_num;
    for (int i = LOAD_HELPER_BEGIN + 1; i < LOAD_HELPER_END; i++) {
        tail.helper_addr[i - LOAD_HELPER_BEGIN - 1] = relkind_to_fixup_addr[i];
    }
    snprintf(tail.aot_version, sizeof(tail.aot_version), "%s", AOT_VERSION);
    if (fwrite(&tail, sizeof(aot_tail), 1, out_tb_file) != 1) {
        fprintf(stderr, "[AOT] Failed to write file tail\n");
        return false;
    }
    if (option_verbose) {
        fprintf(stderr, "[AOT][File] Written: %d pages, %d TBs, "
                "flags=0x%lx, ver=%s\n",
                tail.page_num, tail.tb_num, tail.flags, tail.aot_version);
    }
    return true;
}

bool test_integrity(int fd, uint32_t aot_mode)
{
    aot_tail *tail = get_aot_tail(fd);
    if (!tail) {
        return false;
    }
    if (!memchr(tail->aot_version, '\0', sizeof(tail->aot_version)) ||
            strcmp(tail->aot_version, AOT_VERSION) != 0) {
        fprintf(stderr, "version mismatch in AOT file\n");
        free(tail);
        return false;
    }
    if (tail->flags != get_option_flags(aot_mode)) {
        fprintf(stderr, "flags not equal aot flags %lx curr flags %lx\n",
                tail->flags, get_option_flags(aot_mode));
        free(tail);
        return false;
    }
    uint32_t njc_bits = option_intb_njc &&
        option_jmp_cache_hash != LATX_JMP_CACHE_HASH_PAGE ?
        latx_jmp_cache_bits() : 0;
    if (tail->njc_bits != njc_bits) {
        fprintf(stderr, "njcbits not equal aot value %u curr value %u\n",
                tail->njc_bits, njc_bits);
        free(tail);
        return false;
    }
    if (tail->sys_flag_reduction_tu != option_sys_flag_reduction_tu) {
        fprintf(stderr, "sysflagtu not equal aot value %x curr value %x\n",
                tail->sys_flag_reduction_tu,
                option_sys_flag_reduction_tu);
        free(tail);
        return false;
    }
    if (tail->aot_tu_codegen_flags != get_aot_tu_codegen_flags(aot_mode)) {
        fprintf(stderr, "AOT TU codegen flags not equal aot value %x "
                "curr value %x\n", tail->aot_tu_codegen_flags,
                get_aot_tu_codegen_flags(aot_mode));
        free(tail);
        return false;
    }
    for (int i = LOAD_HELPER_BEGIN + 1; i < LOAD_HELPER_END; i++) {
        if (tail->helper_addr[i - LOAD_HELPER_BEGIN - 1] !=
                relkind_to_fixup_addr[i]) {
            fprintf(stderr, "helper adrr not equal %lx %lx\n",
                    tail->helper_addr[i - LOAD_HELPER_BEGIN - 1],
                    relkind_to_fixup_addr[i]);
            free(tail);
            return false;
        }
    }
    pre_file_tb_count = tail->tb_num;
    if (option_verbose) {
        fprintf(stderr, "[AOT][File] Integrity OK: %d pages, %d TBs, ver=%s\n",
            tail->page_num, tail->tb_num, tail->aot_version);
    }
    free(tail);
    return true;
}

bool validate_aot_layout(const void *mem_ptr, uint64_t file_size)
{
    size_t page_info_size;
    const uint8_t *file = mem_ptr;
    const aot_tail *tail;
    const page_info_array_t *info_array;
    uint64_t page_data_size;
    uint64_t expected_offset = 0;
    const uint32_t min_page_size = sizeof(page_head) + TARGET_PAGE_SIZE;
    bool tu_format;

    if (file_size < sizeof(aot_tail)) {
        return false;
    }

    tail = (const aot_tail *)(file + file_size - sizeof(aot_tail));
    if (tail->page_num < 0 || tail->tb_num < 0 ||
            !aot_page_info_size(tail->page_num, &page_info_size) ||
            page_info_size > file_size - sizeof(aot_tail)) {
        return false;
    }

    info_array = (const page_info_array_t *)((const uint8_t *)tail -
            page_info_size);
    if (info_array->page_count != (uint32_t)tail->page_num) {
        return false;
    }

    tu_format = (tail->flags &
            (UINT64_C(1) << AOT_TU_FLAG_BIT)) != 0;
    page_data_size = (const uint8_t *)info_array - file;
    for (uint32_t i = 0; i < info_array->page_count; i++) {
        uint64_t offset = info_array->pages[i].file_offset;
        uint64_t record_offset;
        uint64_t page_end;
        uint32_t page_record_count = 0;
        const page_head *page;

        if (offset != expected_offset || offset > page_data_size ||
                page_data_size - offset < min_page_size) {
            return false;
        }
        page = (const page_head *)(file + offset);
        if ((page->page_magic != AOT_PAGE_MAGIC &&
                page->page_magic != AOT_MERGED_MAGIC) ||
                page->page_hash != info_array->pages[i].page_hash ||
                page->curr_page_sum_size < min_page_size ||
                (uint64_t)page->curr_page_sum_size >
                    page_data_size - offset) {
            return false;
        }

        page_end = offset + page->curr_page_sum_size;
        record_offset = offset + min_page_size;
        while (page_record_count < page->tb_count) {
            if (record_offset > page_end) {
                return false;
            }

            if (tu_format) {
                const aot_tu_head *tu;
                const aot_tu_page *tu_pages;
                const aot_block *blocks;
                size_t page_bytes;
                size_t block_bytes;
                size_t rel_bytes;
                uint64_t minimum_size;
                uint64_t expected_size;
                bool has_anchor_page = false;

                if (page_end - record_offset >= sizeof(aot_tu_ref)) {
                    const aot_tu_ref *ref =
                        (const aot_tu_ref *)(file + record_offset);

                    if (ref->ref_magic == AOT_TU_REF_MAGIC) {
                        if (ref->ref_size != sizeof(*ref) || !ref->tu_id ||
                                ref->anchor_delta == INT64_MIN ||
                                (ref->anchor_delta &
                                 (TARGET_PAGE_SIZE - 1)) != 0 ||
                                ref->reserved[0] || ref->reserved[1]) {
                            fprintf(stderr, "[AOT] Layout: invalid TU ref "
                                    "page=%u record=%u\n", i,
                                    page_record_count);
                            return false;
                        }
                        record_offset += sizeof(*ref);
                        page_record_count++;
                        continue;
                    }
                }

                if (page_end - record_offset < sizeof(*tu)) {
                    fprintf(stderr, "[AOT] Layout: truncated TU header page=%u\n", i);
                    return false;
                }
                tu = (const aot_tu_head *)(file + record_offset);
                if (tu->tb_magic != AOT_TB_MAGIC || tu->tb_count == 0 ||
                        tu->page_count == 0 ||
                        tu->page_count > AOT_TU_MAX_PAGES ||
                        !tu->tu_id ||
                        tu->reserved != 0 ||
                        __builtin_mul_overflow((size_t)tu->page_count,
                            sizeof(*tu_pages), &page_bytes) ||
                        __builtin_mul_overflow((size_t)tu->tb_count,
                            sizeof(*blocks), &block_bytes) ||
                        __builtin_mul_overflow((size_t)tu->rel_count,
                            sizeof(aot_rel), &rel_bytes) ||
                        __builtin_add_overflow(sizeof(*tu), page_bytes,
                            &minimum_size) ||
                        __builtin_add_overflow(minimum_size, block_bytes,
                            &minimum_size) ||
                        minimum_size > tu->tu_sum_size ||
                        tu->tu_sum_size > page_end - record_offset) {
                    fprintf(stderr, "[AOT] Layout: invalid TU header page=%u "
                            "seen=%u page_tbs=%u tu_tbs=%u sum=%u remain=%llu\n",
                            i, page_record_count, page->tb_count, tu->tb_count,
                            tu->tu_sum_size,
                            (unsigned long long)(page_end - record_offset));
                    return false;
                }

                tu_pages = (const aot_tu_page *)(tu + 1);
                blocks = (const aot_block *)((const uint8_t *)tu_pages +
                        page_bytes);
                for (uint32_t j = 0; j < tu->page_count; j++) {
                    if (tu_pages[j].pc_delta == INT64_MIN ||
                            (tu_pages[j].pc_delta & ~TARGET_PAGE_MASK) != 0 ||
                            (j && tu_pages[j - 1].pc_delta >=
                                  tu_pages[j].pc_delta)) {
                        fprintf(stderr, "[AOT] Layout: invalid TU pages "
                                "page=%u ref=%u delta=%lld\n", i, j,
                                (long long)tu_pages[j].pc_delta);
                        return false;
                    }
                    has_anchor_page |= tu_pages[j].pc_delta == 0;
                }
                if (!has_anchor_page) {
                    fprintf(stderr, "[AOT] Layout: TU page index missing "
                            "page=%u anchor=%d\n", i, has_anchor_page);
                    return false;
                }
                if (blocks[0].pc != tu->first_pc) {
                    fprintf(stderr, "[AOT] Layout: TU entry mismatch page=%u "
                            "first=%" PRIx64 " block=%" PRIx64 "\n", i,
                            (uint64_t)tu->first_pc, (uint64_t)blocks[0].pc);
                    return false;
                }
                if (blocks[0].tu_size < 0 ||
                        __builtin_add_overflow(minimum_size,
                            (uint64_t)blocks[0].tu_size, &minimum_size) ||
                        __builtin_add_overflow(minimum_size,
                            (uint64_t)rel_bytes, &minimum_size) ||
                        minimum_size > tu->tu_sum_size) {
                    fprintf(stderr, "[AOT] Layout: invalid TU payload page=%u "
                            "code=%d rels=%u minimum=%llu sum=%u\n",
                            i, blocks[0].tu_size, tu->rel_count,
                            (unsigned long long)minimum_size, tu->tu_sum_size);
                    return false;
                }
                expected_size = minimum_size;
                for (uint32_t j = 0; j < tu->tb_count; j++) {
                    uint64_t ir2_end;
                    uint64_t block_ir2_size;
                    uint64_t search_size = tail->flags &
                            (UINT64_C(1) << SPLIT_SEARCH_BIT) ?
                        (uint64_t)blocks[j].icount * 4 :
                        blocks[j].search_size;
                    bool invalid_generation_patch = false;
                    bool has_block_page = false;
                    bool has_block_page1 = blocks[j].page_addr[1] ==
                        (tb_page_addr_t)-1;
                    int64_t block_page_delta = (int64_t)(
                        (blocks[j].pc & TARGET_PAGE_MASK) -
                        (tu->first_pc & TARGET_PAGE_MASK));

                    for (uint32_t k = 0; k < tu->page_count; k++) {
                        has_block_page |=
                            tu_pages[k].pc_delta == block_page_delta;
                        if (!has_block_page1) {
                            has_block_page1 = tu_pages[k].pc_delta ==
                                block_page_delta + TARGET_PAGE_SIZE;
                        }
                    }
                    if (!has_block_page || !has_block_page1) {
                        fprintf(stderr, "[AOT] Layout: TU block dependency "
                                "missing page=%u block=%u delta=%lld "
                                "page1=%d\n", i, j,
                                (long long)block_page_delta,
                                has_block_page1);
                        return false;
                    }

                    if (__builtin_mul_overflow(
                            (uint64_t)blocks[j].ir2_num, UINT64_C(4),
                            &block_ir2_size)) {
                        return false;
                    }
                    for (int k = 0; k < 2; k++) {
                        uint16_t patch =
                            blocks[j].tu_generation_offset_patch[k];
                        uint64_t patch_end;

                        if (patch != TB_JMP_OFFSET_INVALID &&
                                ((patch & 3) ||
                                 __builtin_add_overflow((uint64_t)patch,
                                     UINT64_C(8), &patch_end) ||
                                 patch_end > block_ir2_size)) {
                            invalid_generation_patch = true;
                        }
                    }

                    if (invalid_generation_patch ||
                            __builtin_add_overflow(expected_size, search_size,
                                &expected_size) ||
                            ((tail->flags & (UINT64_C(1) << PUSHPOP)) &&
                             __builtin_add_overflow(expected_size,
                                 (uint64_t)blocks[j].icount * 4,
                                 &expected_size)) ||
                            __builtin_add_overflow(
                                (uint64_t)blocks[j].tc.offset_in_tu,
                                block_ir2_size, &ir2_end) ||
                            ir2_end > (uint64_t)blocks[0].tu_size ||
                            (blocks[j].rel_start < 0 ?
                                (blocks[j].rel_start != -1 ||
                                 blocks[j].rel_end != -2) :
                                (blocks[j].rel_start < 0 ||
                                 blocks[j].rel_end < blocks[j].rel_start ||
                                 (uint32_t)blocks[j].rel_end >= tu->rel_count))) {
                        fprintf(stderr, "[AOT] Layout: invalid TU block page=%u "
                                "block=%u offset=%zu ir2=%u code=%d rel=%d..%d/%u\n",
                                i, j, blocks[j].tc.offset_in_tu,
                                blocks[j].ir2_num, blocks[0].tu_size,
                                blocks[j].rel_start, blocks[j].rel_end,
                                tu->rel_count);
                        return false;
                    }
                }
                if (expected_size != tu->tu_sum_size) {
                    fprintf(stderr, "[AOT] Layout: TU payload size mismatch "
                            "page=%u expected=%llu sum=%u\n", i,
                            (unsigned long long)expected_size,
                            tu->tu_sum_size);
                    return false;
                }

                record_offset += tu->tu_sum_size;
                page_record_count++;
            } else {
                const aot_block *block;
                uint32_t magic, sum_size, all_tb_size, rel_count;
                size_t rel_bytes;
                uint64_t expected_size;
                uint64_t ir2_size;

                if (page_end - record_offset < 4 * sizeof(uint32_t)) {
                    return false;
                }
                memcpy(&magic, file + record_offset, sizeof(magic));
                memcpy(&sum_size, file + record_offset + 4,
                        sizeof(sum_size));
                memcpy(&all_tb_size, file + record_offset + 8,
                        sizeof(all_tb_size));
                memcpy(&rel_count, file + record_offset + 12,
                        sizeof(rel_count));
                if (magic != AOT_TB_MAGIC ||
                        all_tb_size < sizeof(*block) ||
                        __builtin_mul_overflow((size_t)rel_count,
                            sizeof(aot_rel), &rel_bytes) ||
                        __builtin_add_overflow(
                            (uint64_t)(4 * sizeof(uint32_t) + all_tb_size),
                            (uint64_t)rel_bytes, &expected_size) ||
                        expected_size != sum_size ||
                        sum_size > page_end - record_offset) {
                    return false;
                }
                block = (const aot_block *)(file + record_offset + 16);
                if (__builtin_mul_overflow((uint64_t)block->ir2_num,
                        UINT64_C(4), &ir2_size) ||
                        ir2_size > all_tb_size - sizeof(*block)) {
                    return false;
                }

                record_offset += sum_size;
                page_record_count++;
            }
        }
        if (record_offset != page_end) {
            fprintf(stderr, "[AOT] Layout: page records do not fill page=%u "
                    "offset=%llu end=%llu\n", i,
                    (unsigned long long)record_offset,
                    (unsigned long long)page_end);
            return false;
        }
        expected_offset = page_end;
    }

    return expected_offset == page_data_size &&
        (!tu_format || validate_aot_tu_references(file, info_array));
}

static uint32_t hash_5_to_32(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
        uint32_t e)
{
    uint32_t seed = 0x9E3779B9;
    uint32_t h = seed;
    h = (h << 5) | (h >> 27);
    h ^= a + 0x5bd1e995;
    h *= 0x5bd1e995;

    h = (h << 7) | (h >> 25);
    h ^= b;
    h *= 0xcc9e2d51;

    h = (h << 11) | (h >> 21);
    h ^= c;
    h += d;

    h ^= e;

    return h;
}

static uint32_t hash_func(uint32_t pc, uint32_t code1, uint32_t code2,
        uint32_t code3, uint32_t code4)
{
    uint32_t prime1 = 2654435761U;
    uint32_t prime2 = 2246822519U;
    uint32_t prime3 = 3266489917U;
    uint32_t prime4 =  668265263U;
    uint32_t prime5 =  374761393U;
    uint32_t hash;
    hash = (rol32(pc * prime1, 1) + rol32(code1 * prime2, 12) +
            rol32(code2 * prime3, 16));
    hash += code3 * prime4;
    hash = rol32(hash, 17) * prime5;
    hash += code4 * prime4;
    hash = rol32(hash, 17) * prime5;

    hash ^= hash >> 16;
    hash *= prime2;
    hash ^= hash >> 10;
    return hash;
}

uint32_t get_page_hash(uint32_t *guest_code)
{
    uint32_t hash = hash_5_to_32(guest_code[2], guest_code[307],
            guest_code[1009], guest_code[731], guest_code[881]);
    return hash_func(hash, guest_code[683], guest_code[543],
            guest_code[1023], guest_code[0]);
}

uint64_t get_page_fingerprint(const void *guest_code)
{
    const uint64_t *words = guest_code;
    uint64_t v1 = QEMU_XXHASH_SEED + XXH_PRIME64_1 + XXH_PRIME64_2;
    uint64_t v2 = QEMU_XXHASH_SEED + XXH_PRIME64_2;
    uint64_t v3 = QEMU_XXHASH_SEED;
    uint64_t v4 = QEMU_XXHASH_SEED - XXH_PRIME64_1;

    for (size_t i = 0; i < TARGET_PAGE_SIZE / sizeof(*words); i += 4) {
        v1 = XXH64_round(v1, words[i]);
        v2 = XXH64_round(v2, words[i + 1]);
        v3 = XXH64_round(v3, words[i + 2]);
        v4 = XXH64_round(v4, words[i + 3]);
    }
    return XXH64_avalanche(XXH64_mergerounds(v1, v2, v3, v4) +
                           TARGET_PAGE_SIZE);
}
