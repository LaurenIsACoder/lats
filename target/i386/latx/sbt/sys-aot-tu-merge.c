/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Cross-page AOT merge: plan globally, translate to spool, then emit pages. */
#include "sys-aot.h"
#include "sys-aot-internal.h"
#include "sys-aot-tu-merge.h"
#include "sys-aot-tu-codec.h"
#include "latx-tu.h"
#include <limits.h>

__thread AOTFileCodeView *aot_file_code_view;
__thread CPUState *aot_tu_merge_cpu;

const uint8_t *aot_file_code_page(uint64_t pc)
{
    uint64_t page = pc & TARGET_PAGE_MASK;
    size_t low = 0, high = aot_file_code_view->count;

    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const AOTFileCodePage *candidate = &aot_file_code_view->pages[mid];

        if (candidate->pc == page) {
            return candidate->bytes;
        }
        if (candidate->pc < page) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return NULL;
}

#ifdef CONFIG_LATX_TU
typedef struct MergePage {
    const uint8_t *bytes;
    uint32_t hash;
    uint64_t fingerprint;
} MergePage;

typedef struct MergeGroup MergeGroup;

typedef struct MergeTU {
    const aot_tu_head *head;
    const aot_tu_page *deps;
    const aot_block *blocks;
    MergePage *pages[AOT_TU_MAX_PAGES];
    MergeGroup *group;
    uint64_t bias;
} MergeTU;

typedef struct MergeMapping {
    uint64_t pc;
    MergePage *page;
} MergeMapping;

struct MergeGroup {
    GPtrArray *tus;
    GArray *mappings;
    GHashTable *page_index;
    GHashTable *member_index;
};

typedef struct MergeRefKey {
    uint64_t id;
    int64_t delta;
} MergeRefKey;

typedef struct MergeMember {
    MergeTU *tu;
    const aot_block *block;
    MergePage *page;
} MergeMember;

typedef struct MergeMemberPosition {
    uint64_t pc;
    bool ambiguous;
} MergeMemberPosition;

typedef struct MergeOutput {
    const uint8_t *original;
    off_t offset;
    uint32_t size;
    uint32_t tb_count;
    uint64_t id;
    uint64_t anchor;
    uint32_t page_count;
    MergeMapping pages[AOT_TU_MAX_PAGES];
} MergeOutput;

typedef struct MergeOutputRef {
    MergeOutput *tu;
    int64_t delta;
} MergeOutputRef;

typedef struct MergePlan {
    GHashTable *pages;
    GPtrArray *tus;
    GPtrArray *groups;
    GPtrArray *outputs;
    FILE *spool;
    uint8_t *copy_buffer;
} MergePlan;

#define MERGE_COPY_BUFFER_SIZE (64 * 1024)

static guint merge_page_hash(gconstpointer p)
{
    return ((const MergePage *)p)->hash;
}

static gboolean merge_page_equal(gconstpointer ap, gconstpointer bp)
{
    const MergePage *a = ap, *b = bp;

    return a->hash == b->hash &&
        !memcmp(a->bytes, b->bytes, TARGET_PAGE_SIZE);
}

static MergePage *merge_page_intern(MergePlan *plan, const uint8_t *bytes)
{
    MergePage key = { .bytes = bytes };
    MergePage *page;

    key.hash = get_page_hash((uint32_t *)bytes);
    page = g_hash_table_lookup(plan->pages, &key);
    if (!page) {
        key.fingerprint = get_page_fingerprint(bytes);
        page = g_memdup2(&key, sizeof(key));
        g_hash_table_add(plan->pages, page);
    }
    return page;
}

static guint merge_ref_hash(gconstpointer p)
{
    const MergeRefKey *key = p;
    uint64_t h = key->id ^ (uint64_t)key->delta;

    return h ^ (h >> 32);
}

static gboolean merge_ref_equal(gconstpointer ap, gconstpointer bp)
{
    const MergeRefKey *a = ap, *b = bp;

    return a->id == b->id && a->delta == b->delta;
}

static MergePage *merge_mapping_page(const GArray *mappings, uint64_t pc)
{
    guint low = 0, high = mappings->len;

    /* Translation sorts the complete group before looking up output pages. */
    while (low < high) {
        guint mid = low + (high - low) / 2;
        const MergeMapping *m = &g_array_index(mappings, MergeMapping, mid);

        if (m->pc == pc) {
            return m->page;
        }
        if (m->pc < pc) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return NULL;
}

static void merge_group_free(gpointer opaque)
{
    MergeGroup *group = opaque;

    g_ptr_array_free(group->tus, true);
    g_array_free(group->mappings, true);
    if (group->page_index) {
        g_hash_table_destroy(group->page_index);
    }
    if (group->member_index) {
        g_hash_table_destroy(group->member_index);
    }
    g_free(group);
}

/* Validated reference keys locate the exact member page, not a hash guess. */
static bool merge_parse_input(MergePlan *plan, const uint8_t *file,
                              uint64_t size)
{
    const aot_tail *tail = (const aot_tail *)(file + size - sizeof(*tail));
    const page_info_array_t *index = (const page_info_array_t *)(
        (const uint8_t *)tail - sizeof(*index) -
        tail->page_num * sizeof(page_info_t));
    GHashTable *refs = g_hash_table_new_full(merge_ref_hash, merge_ref_equal,
                                            g_free, NULL);
    guint first = plan->tus->len;
    bool ok = false;

    for (uint32_t i = 0; i < index->page_count; i++) {
        const page_head *ph = (const page_head *)(file +
                                                index->pages[i].file_offset);
        const uint8_t *record = (const uint8_t *)(ph + 1) + TARGET_PAGE_SIZE;
        MergePage *page = merge_page_intern(plan, (const uint8_t *)(ph + 1));

        for (uint32_t j = 0; j < ph->tb_count; j++) {
            if (*(const uint32_t *)record == AOT_TB_MAGIC) {
                MergeTU *tu = g_new0(MergeTU, 1);

                tu->head = (const aot_tu_head *)record;
                tu->deps = (const aot_tu_page *)(tu->head + 1);
                tu->blocks = (const aot_block *)(tu->deps +
                                                tu->head->page_count);
                for (uint32_t k = 0; k < tu->head->page_count; k++) {
                    if (!tu->deps[k].pc_delta) {
                        tu->pages[k] = page;
                    }
                }
                g_ptr_array_add(plan->tus, tu);
                record += tu->head->tu_sum_size;
            } else {
                const aot_tu_ref *ref = (const aot_tu_ref *)record;
                MergeRefKey key = { ref->tu_id, ref->anchor_delta };

                if (g_hash_table_contains(refs, &key)) {
                    goto out;
                }
                g_hash_table_insert(refs, g_memdup2(&key, sizeof(key)), page);
                record += sizeof(*ref);
            }
        }
    }
    for (guint i = first; i < plan->tus->len; i++) {
        MergeTU *tu = g_ptr_array_index(plan->tus, i);
        MergeGroup *group = g_new0(MergeGroup, 1);
        uint64_t anchor = tu->head->first_pc & TARGET_PAGE_MASK;

        group->tus = g_ptr_array_new();
        group->mappings = g_array_new(false, false, sizeof(MergeMapping));
        g_ptr_array_add(plan->groups, group);
        g_ptr_array_add(group->tus, tu);
        tu->group = group;
        for (uint32_t k = 0; k < tu->head->page_count; k++) {
            MergeRefKey key = { tu->head->tu_id, -tu->deps[k].pc_delta };
            MergeMapping m = { .pc = anchor + tu->deps[k].pc_delta };

            if (tu->deps[k].pc_delta) {
                tu->pages[k] = g_hash_table_lookup(refs, &key);
            }
            m.page = tu->pages[k];
            if (!m.page ||
                m.page->fingerprint != tu->deps[k].page_fingerprint) {
                goto out;
            }
            g_array_append_val(group->mappings, m);
        }
    }
    ok = true;
out:
    g_hash_table_destroy(refs);
    return ok;
}

static MergePage *merge_tu_block_page(const MergeTU *tu, uint64_t pc)
{
    uint64_t delta = (pc & TARGET_PAGE_MASK) -
                    (tu->head->first_pc & TARGET_PAGE_MASK);

    for (uint32_t i = 0; i < tu->head->page_count; i++) {
        if ((uint64_t)tu->deps[i].pc_delta == delta) {
            return tu->pages[i];
        }
    }
    return NULL;
}

static guint merge_member_hash(gconstpointer opaque)
{
    const MergeMember *m = opaque;
    const aot_block *b = m->block;

    return m->page->hash ^ (b->pc & ~TARGET_PAGE_MASK) ^ b->flags ^
        b->cflags ^ b->cs_base;
}

static gboolean merge_member_equal(gconstpointer ap, gconstpointer bp)
{
    const MergeMember *a = ap, *b = bp;

    return a->page == b->page &&
        (a->block->pc & ~TARGET_PAGE_MASK) ==
        (b->block->pc & ~TARGET_PAGE_MASK) &&
        a->block->flags == b->block->flags &&
        a->block->cflags == b->block->cflags &&
        a->block->cs_base == b->block->cs_base;
}

/* Index a singleton before it participates in any joins. */
static bool merge_group_index(MergeTU *tu)
{
    MergeGroup *group = tu->group;

    group->page_index = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                             g_free, NULL);
    group->member_index = g_hash_table_new_full(merge_member_hash,
        merge_member_equal, g_free, g_free);
    for (guint i = 0; i < group->mappings->len; i++) {
        MergeMapping *m = &g_array_index(group->mappings, MergeMapping, i);

        g_hash_table_insert(group->page_index,
                            g_memdup2(&m->pc, sizeof(m->pc)), m->page);
    }
    for (uint32_t i = 0; i < tu->head->tb_count; i++) {
        MergeMember member = { tu, &tu->blocks[i],
            merge_tu_block_page(tu, tu->blocks[i].pc) };
        MergeMemberPosition *position;

        if (!member.page) {
            return false;
        }
        position = g_hash_table_lookup(group->member_index, &member);
        if (position) {
            position->ambiguous |= position->pc != member.block->pc;
        } else {
            position = g_new0(MergeMemberPosition, 1);
            position->pc = member.block->pc;
            g_hash_table_insert(group->member_index,
                                g_memdup2(&member, sizeof(member)), position);
        }
    }
    return true;
}

/* Repeated code at different relative addresses cannot choose an alignment. */
static bool merge_member_unambiguous(const MergeMember *member)
{
    MergeMemberPosition *position = g_hash_table_lookup(
        member->tu->group->member_index, member);

    g_assert(position);
    return !position->ambiguous;
}

static bool merge_same_tu(const MergeTU *a, const MergeTU *b)
{
    if ((a->head->first_pc & ~TARGET_PAGE_MASK) !=
        (b->head->first_pc & ~TARGET_PAGE_MASK) ||
        a->head->tu_id != b->head->tu_id ||
        a->head->tb_count != b->head->tb_count ||
        a->head->page_count != b->head->page_count ||
        memcmp(a->deps, b->deps, a->head->page_count * sizeof(*a->deps))) {
        return false;
    }
    for (uint32_t i = 0; i < a->head->page_count; i++) {
        if (a->pages[i] != b->pages[i]) {
            return false;
        }
    }
    for (uint32_t i = 0; i < a->head->tb_count; i++) {
        const aot_block *ab = &a->blocks[i], *bb = &b->blocks[i];

        if (ab->pc - a->head->first_pc != bb->pc - b->head->first_pc ||
            ab->flags != bb->flags || ab->cflags != bb->cflags ||
            ab->cs_base != bb->cs_base || ab->size != bb->size ||
            ab->icount != bb->icount) {
            return false;
        }
    }
    return true;
}

/* Align through an actual common TB and validate the whole resulting map. */
static void merge_join_members(MergeMember *a, MergeMember *b)
{
    MergeGroup *ga = a->tu->group, *gb = b->tu->group;
    uint64_t shift = a->block->pc + a->tu->bias -
                     b->block->pc - b->tu->bias;

    if (ga == gb) {
        return;
    }
    if (merge_same_tu(a->tu, b->tu)) {
        /* The full relative layout disambiguates repeated identical pages. */
        shift = a->tu->head->first_pc + a->tu->bias -
                b->tu->head->first_pc - b->tu->bias;
    } else if (!merge_member_unambiguous(a) || !merge_member_unambiguous(b)) {
        return;
    }
    /* Each member moves only when its containing group at least doubles. */
    if (ga->tus->len < gb->tus->len) {
        MergeGroup *tmp = ga;

        ga = gb;
        gb = tmp;
        shift = -shift;
    }
    for (guint i = 0; i < gb->mappings->len; i++) {
        MergeMapping m = g_array_index(gb->mappings, MergeMapping, i);
        uint64_t pc = m.pc + shift;
        MergePage *existing = g_hash_table_lookup(ga->page_index, &pc);

        if (existing && existing != m.page) {
            return; /* Distinct dependency versions remain distinct TUs. */
        }
    }
    for (guint i = 0; i < gb->mappings->len; i++) {
        MergeMapping m = g_array_index(gb->mappings, MergeMapping, i);

        m.pc += shift;
        if (!g_hash_table_contains(ga->page_index, &m.pc)) {
            g_array_append_val(ga->mappings, m);
            g_hash_table_insert(ga->page_index,
                                g_memdup2(&m.pc, sizeof(m.pc)), m.page);
        }
    }
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, gb->member_index);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        MergeMemberPosition *position = value;
        MergeMemberPosition *existing = g_hash_table_lookup(ga->member_index,
                                                            key);

        position->pc += shift;
        if (existing) {
            existing->ambiguous |= position->ambiguous ||
                                   existing->pc != position->pc;
            g_hash_table_iter_remove(&iter);
        } else {
            g_hash_table_iter_steal(&iter);
            g_hash_table_insert(ga->member_index, key, value);
        }
    }
    for (guint i = 0; i < gb->tus->len; i++) {
        MergeTU *tu = g_ptr_array_index(gb->tus, i);

        tu->bias += shift;
        tu->group = ga;
        g_ptr_array_add(ga->tus, tu);
    }
    g_ptr_array_set_size(gb->tus, 0);
    g_array_set_size(gb->mappings, 0);
    g_clear_pointer(&gb->page_index, g_hash_table_destroy);
    g_clear_pointer(&gb->member_index, g_hash_table_destroy);
}

static bool merge_plan_groups(MergePlan *plan)
{
    GHashTable *members = g_hash_table_new_full(merge_member_hash,
        merge_member_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    GHashTable *identities = g_hash_table_new_full(g_int64_hash, g_int64_equal,
        g_free, (GDestroyNotify)g_ptr_array_unref);
    bool ok = false;

    for (guint i = 0; i < plan->tus->len; i++) {
        MergeTU *tu = g_ptr_array_index(plan->tus, i);

        if (!merge_group_index(tu)) {
            goto out;
        }
        /*
         * A representative member cannot disambiguate repeated page bytes.
         * Keep exact-layout lookup independent of the pruned member buckets.
         * The ID only selects candidates; compare the complete identity.
         */
        GPtrArray *same_id = g_hash_table_lookup(identities, &tu->head->tu_id);
        bool duplicate = false;

        if (!same_id) {
            same_id = g_ptr_array_new();
            g_hash_table_insert(identities,
                g_memdup2(&tu->head->tu_id, sizeof(tu->head->tu_id)), same_id);
        }
        for (guint j = 0; j < same_id->len; j++) {
            MergeTU *previous = g_ptr_array_index(same_id, j);

            if (merge_same_tu(previous, tu)) {
                MergeMember a = { previous, &previous->blocks[0],
                    merge_tu_block_page(previous, previous->blocks[0].pc) };
                MergeMember b = { tu, &tu->blocks[0],
                    merge_tu_block_page(tu, tu->blocks[0].pc) };

                merge_join_members(&a, &b);
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            g_ptr_array_add(same_id, tu);
        }
        for (uint32_t j = 0; j < tu->head->tb_count; j++) {
            MergeMember *m = g_new0(MergeMember, 1);
            GPtrArray *bucket;

            m->tu = tu;
            m->block = &tu->blocks[j];
            m->page = merge_tu_block_page(tu, m->block->pc);
            if (!m->page) {
                g_free(m);
                goto out;
            }
            bucket = g_hash_table_lookup(members, m);
            if (!bucket) {
                bucket = g_ptr_array_new_with_free_func(g_free);
                g_hash_table_insert(members, g_memdup2(m, sizeof(*m)), bucket);
            } else {
                for (guint k = 0; k < bucket->len; ) {
                    MergeMember *other = g_ptr_array_index(bucket, k);

                    merge_join_members(other, m);
                    if (other->tu->group == m->tu->group) {
                        /* Retain one candidate per joined group, not per TU. */
                        g_ptr_array_remove_index_fast(bucket, k);
                    } else {
                        k++;
                    }
                }
            }
            g_ptr_array_add(bucket, m);
        }
    }
    ok = true;
out:
    g_hash_table_destroy(identities);
    g_hash_table_destroy(members);
    return ok;
}

static void merge_keep_tu(MergePlan *plan, const MergeTU *tu)
{
    MergeOutput *out = g_new0(MergeOutput, 1);

    out->original = (const uint8_t *)tu->head;
    out->size = tu->head->tu_sum_size;
    out->tb_count = tu->head->tb_count;
    out->id = tu->head->tu_id;
    out->anchor = tu->head->first_pc & TARGET_PAGE_MASK;
    out->page_count = tu->head->page_count;
    for (uint32_t i = 0; i < out->page_count; i++) {
        out->pages[i] = (MergeMapping) {
            out->anchor + tu->deps[i].pc_delta, tu->pages[i],
        };
    }
    g_ptr_array_add(plan->outputs, out);
}

/* A later increment may already be a subset of a previously merged TU. */
static MergeTU *merge_covering_tu(const MergeGroup *group)
{
    MergeTU *candidate = g_ptr_array_index(group->tus, 0);

    for (guint i = 1; i < group->tus->len; i++) {
        MergeTU *tu = g_ptr_array_index(group->tus, i);

        if (tu->head->tb_count > candidate->head->tb_count) {
            candidate = tu;
        }
    }
    for (guint i = 0; i < group->tus->len; i++) {
        MergeTU *tu = g_ptr_array_index(group->tus, i);

        if (tu == candidate) {
            continue;
        }
        for (uint32_t j = 0; j < tu->head->tb_count; j++) {
            const aot_block *block = &tu->blocks[j];
            bool found = false;

            for (uint32_t k = 0; k < candidate->head->tb_count; k++) {
                const aot_block *other = &candidate->blocks[k];

                if (block->pc + tu->bias == other->pc + candidate->bias &&
                    block->cs_base == other->cs_base &&
                    block->flags == other->flags &&
                    block->cflags == other->cflags &&
                    block->size == other->size &&
                    block->icount == other->icount) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return NULL;
            }
        }
    }
    return candidate;
}

typedef struct MergeTranslate {
    MergePlan *plan;
    MergeGroup *group;
    GPtrArray *templates;
    GHashTable *requested;
    GHashTable *covered;
} MergeTranslate;

static guint merge_tb_hash(gconstpointer opaque)
{
    const TranslationBlock *tb = opaque;

    return tb->pc ^ (tb->pc >> 32) ^ tb->cs_base ^ tb->flags ^ tb->cflags;
}

static gboolean merge_tb_equal(gconstpointer ap, gconstpointer bp)
{
    const TranslationBlock *a = ap, *b = bp;

    return a->pc == b->pc && a->cs_base == b->cs_base &&
        a->flags == b->flags && a->cflags == b->cflags;
}

static void merge_template_free(gpointer opaque)
{
    TranslationBlock *tb = opaque;

    g_free(tb->s_data);
    g_free(tb);
}

static gint merge_mapping_cmp(gconstpointer ap, gconstpointer bp)
{
    const MergeMapping *a = ap, *b = bp;

    return a->pc < b->pc ? -1 : a->pc > b->pc;
}

static AOTCrossPageTUResult merge_spool_tu(TranslationBlock **tbs,
                                          int count, void *opaque)
{
    MergeTranslate *tr = opaque;
    MergeOutput *out = g_new0(MergeOutput, 1);
    aot_tu_page deps[AOT_TU_MAX_PAGES];
    int size = 0;
    bool ok = false;

    out->anchor = tbs[0]->pc & TARGET_PAGE_MASK;
    out->tb_count = count;
    for (int i = 0; i < count; i++) {
        TranslationBlock *tb = tbs[i];
        uint64_t first = tb->pc & TARGET_PAGE_MASK;
        uint64_t last = (tb->pc + tb->size - 1) & TARGET_PAGE_MASK;

        for (uint64_t pc = first; ; pc += TARGET_PAGE_SIZE) {
            uint32_t j;

            for (j = 0; j < out->page_count; j++) {
                if (out->pages[j].pc == pc) {
                    break;
                }
            }
            if (j == out->page_count) {
                MergePage *page = merge_mapping_page(tr->group->mappings, pc);

                if (!page || j == AOT_TU_MAX_PAGES) {
                    goto done;
                }
                out->pages[out->page_count++] = (MergeMapping) { pc, page };
            }
            if (pc == last) {
                break;
            }
        }
        /* Every output entry must be one of the requested templates. */
        TranslationBlock *input = g_hash_table_lookup(tr->requested, tb);

        if (!input || !g_hash_table_add(tr->covered, input)) {
            goto done;
        }
    }
    qsort(out->pages, out->page_count, sizeof(*out->pages), merge_mapping_cmp);
    for (uint32_t i = 0; i < out->page_count; i++) {
        deps[i] = (aot_tu_page) {
            (int64_t)(out->pages[i].pc - out->anchor),
            out->pages[i].page->fingerprint,
        };
    }
    out->offset = ftello(tr->plan->spool);
    if (out->offset < 0 ||
        !aot_encode_tu(tr->plan->spool, tbs, count, deps, out->page_count,
                        &size, &out->id)) {
        goto done;
    }
    if (ftello(tr->plan->spool) - out->offset != size) {
        goto done;
    }
    out->size = size;
    g_ptr_array_add(tr->plan->outputs, out);
    ok = true;
done:
    if (!ok) {
        g_free(out);
    }
    aot_reset_merge_translation();
    return ok ? AOT_TU_SAVE_OK : AOT_TU_SAVE_ERROR;
}

static bool merge_translate_group(MergePlan *plan, MergeGroup *group)
{
    MergeTranslate tr = {
        .plan = plan,
        .group = group,
        .templates = g_ptr_array_new_with_free_func(merge_template_free),
        .requested = g_hash_table_new(merge_tb_hash, merge_tb_equal),
        .covered = g_hash_table_new(merge_tb_hash, merge_tb_equal),
    };
    GArray *pages = g_array_new(false, false, sizeof(AOTFileCodePage));
    AOTFileCodeView view = { 0 };
    bool ok = false;

    if (!aot_tu_merge_cpu || aot_file_code_view) {
        goto out;
    }
    g_array_sort(group->mappings, merge_mapping_cmp);
    for (guint i = 0; i < group->mappings->len; i++) {
        MergeMapping m = g_array_index(group->mappings, MergeMapping, i);
        AOTFileCodePage page = { m.pc, m.page->bytes };

        g_array_append_val(pages, page);
    }
    for (guint i = 0; i < group->tus->len; i++) {
        MergeTU *tu = g_ptr_array_index(group->tus, i);

        for (uint32_t j = 0; j < tu->head->tb_count; j++) {
            const aot_block *b = &tu->blocks[j];
            TranslationBlock *tb = g_new0(TranslationBlock, 1);
            TranslationBlock *old;

            tb->pc = b->pc + tu->bias;
            tb->cs_base = b->cs_base;
            tb->flags = b->flags;
            tb->cflags = b->cflags;
            tb->size = b->size;
            tb->icount = b->icount;
            old = g_hash_table_lookup(tr.requested, tb);
            if (old) {
                /* Same bytes and entry, but an earlier TU may have split it. */
                if (tb->size > old->size) {
                    old->size = tb->size;
                    old->icount = tb->icount;
                    tb_set_page_addr1(old, (old->pc & TARGET_PAGE_MASK) ==
                        ((old->pc + old->size - 1) & TARGET_PAGE_MASK) ? -1 :
                        ((old->pc + old->size - 1) & TARGET_PAGE_MASK));
                }
                g_free(tb);
                continue;
            }
            tb->s_data = g_new0(struct separated_data, 1);
            tb_set_page_addr0(tb, tb->pc & TARGET_PAGE_MASK);
            tb_set_page_addr1(tb, (tb->pc & TARGET_PAGE_MASK) ==
                ((tb->pc + tb->size - 1) & TARGET_PAGE_MASK) ? -1 :
                ((tb->pc + tb->size - 1) & TARGET_PAGE_MASK));
            g_hash_table_add(tr.requested, tb);
            g_ptr_array_add(tr.templates, tb);
        }
    }
    if (!tr.templates->len || tr.templates->len > INT_MAX) {
        goto out;
    }
    aot_prepare_merge_translation();
    view.pages = (AOTFileCodePage *)pages->data;
    view.count = pages->len;
    aot_file_code_view = &view;
    ok = retranslate_by_tu_cross_page(aot_tu_merge_cpu,
        (TranslationBlock **)tr.templates->pdata, tr.templates->len,
        merge_spool_tu, &tr) && !view.failed &&
        g_hash_table_size(tr.covered) == tr.templates->len;
    aot_file_code_view = NULL;
    aot_reset_merge_translation();
out:
    g_hash_table_destroy(tr.requested);
    g_hash_table_destroy(tr.covered);
    g_ptr_array_free(tr.templates, true);
    g_array_free(pages, true);
    return ok;
}

static bool merge_write_bytes(FILE *out, const void *bytes, size_t size)
{
    return !size || fwrite(bytes, 1, size, out) == size;
}

static bool merge_copy_output(MergePlan *plan, FILE *file, MergeOutput *tu)
{
    size_t remaining = tu->size;

    if (tu->original) {
        return merge_write_bytes(file, tu->original, remaining);
    }
    /* HAMT fake-fork callbacks have only a 64 KiB host stack. */
    if (!plan->copy_buffer) {
        plan->copy_buffer = g_try_malloc(MERGE_COPY_BUFFER_SIZE);
        if (!plan->copy_buffer) {
            return false;
        }
    }
    if (fseeko(plan->spool, tu->offset, SEEK_SET)) {
        return false;
    }
    while (remaining) {
        size_t n = MIN(remaining, MERGE_COPY_BUFFER_SIZE);

        if (fread(plan->copy_buffer, 1, n, plan->spool) != n ||
            !merge_write_bytes(file, plan->copy_buffer, n)) {
            return false;
        }
        remaining -= n;
    }
    return true;
}

static gint merge_output_page_cmp(gconstpointer ap, gconstpointer bp)
{
    const MergePage *a = *(MergePage * const *)ap;
    const MergePage *b = *(MergePage * const *)bp;

    if (a->hash != b->hash) {
        return a->hash < b->hash ? -1 : 1;
    }
    return memcmp(a->bytes, b->bytes, TARGET_PAGE_SIZE);
}

static bool merge_write_plan(MergePlan *plan, FILE *out, uint32_t mode,
                             int *page_count, int *tb_count)
{
    GHashTable *pages = g_hash_table_new_full(g_direct_hash, g_direct_equal,
        NULL, (GDestroyNotify)g_array_unref);
    GArray *index = g_array_new(false, true, sizeof(page_info_t));
    GPtrArray *ordered_pages = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key, value;
    uint64_t total = 0;
    bool ok = false;

    for (guint i = 0; i < plan->outputs->len; i++) {
        MergeOutput *tu = g_ptr_array_index(plan->outputs, i);

        total += tu->tb_count;
        if (total > INT_MAX) {
            goto done;
        }
        for (uint32_t j = 0; j < tu->page_count; j++) {
            MergePage *page = tu->pages[j].page;
            GArray *refs = g_hash_table_lookup(pages, page);
            MergeOutputRef ref = { tu, tu->anchor - tu->pages[j].pc };

            if (!refs) {
                refs = g_array_new(false, false, sizeof(ref));
                g_hash_table_insert(pages, page, refs);
            }
            g_array_append_val(refs, ref);
        }
    }
    g_hash_table_iter_init(&iter, pages);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_ptr_array_add(ordered_pages, key);
    }
    /* The file layout must not depend on allocator addresses or hash order. */
    g_ptr_array_sort(ordered_pages, merge_output_page_cmp);
    for (guint page_index = 0; page_index < ordered_pages->len; page_index++) {
        MergePage *page = g_ptr_array_index(ordered_pages, page_index);
        GArray *refs = g_hash_table_lookup(pages, page);
        page_head ph = {
            .page_magic = AOT_PAGE_MAGIC,
            .page_hash = page->hash,
            .tb_count = refs->len,
        };
        page_info_t info = { 0 };
        uint64_t size = sizeof(ph) + TARGET_PAGE_SIZE;
        off_t offset = ftello(out);

        for (guint i = 0; i < refs->len; i++) {
            MergeOutputRef ref = g_array_index(refs, MergeOutputRef, i);

            size += ref.delta ? sizeof(aot_tu_ref) : ref.tu->size;
        }
        if (offset < 0 || size > INT_MAX || index->len == INT_MAX) {
            goto done;
        }
        ph.curr_page_sum_size = size;
        info.page_hash = page->hash;
        info.file_offset = offset;
        g_array_append_val(index, info);
        if (!merge_write_bytes(out, &ph, sizeof(ph)) ||
            !merge_write_bytes(out, page->bytes, TARGET_PAGE_SIZE)) {
            goto done;
        }
        for (guint i = 0; i < refs->len; i++) {
            MergeOutputRef ref = g_array_index(refs, MergeOutputRef, i);

            if (ref.delta) {
                aot_tu_ref r = {
                    .ref_magic = AOT_TU_REF_MAGIC,
                    .ref_size = sizeof(r),
                    .tu_id = ref.tu->id,
                    .anchor_delta = ref.delta,
                };

                if (!merge_write_bytes(out, &r, sizeof(r))) {
                    goto done;
                }
            } else if (!merge_copy_output(plan, out, ref.tu)) {
                goto done;
            }
        }
    }
    size_t index_size;
    if (!aot_page_info_size(index->len, &index_size)) {
        goto done;
    }
    page_info_array_t *array = g_malloc0(index_size);

    array->page_count = index->len;
    memcpy(array->pages, index->data, index->len * sizeof(page_info_t));
    ok = merge_write_bytes(out, array, index_size);
    g_free(array);
    if (ok) {
        /* Legacy tail encoder uses the writer bound by the file transaction. */
        g_assert(out_tb_file == out);
        ok = save_aot_tail(index->len, total, mode);
    }
    if (ok) {
        *page_count = index->len;
        *tb_count = total;
    }
done:
    g_ptr_array_free(ordered_pages, true);
    g_array_free(index, true);
    g_hash_table_destroy(pages);
    return ok;
}

bool aot_merge_tu_files(const void *add, uint64_t add_size,
                       const void *base, uint64_t base_size, FILE *out,
                       uint32_t mode, int *pages, int *tbs)
{
    MergePlan plan = {
        .pages = g_hash_table_new_full(merge_page_hash, merge_page_equal,
                                       g_free, NULL),
        .tus = g_ptr_array_new_with_free_func(g_free),
        .groups = g_ptr_array_new_with_free_func(merge_group_free),
        .outputs = g_ptr_array_new_with_free_func(g_free),
    };
    bool ok = false;
    unsigned retranslated = 0;

    plan.spool = tmpfile();
    if (!plan.spool || !merge_parse_input(&plan, add, add_size) ||
        !merge_parse_input(&plan, base, base_size) ||
        !merge_plan_groups(&plan)) {
        goto done;
    }
    for (guint i = 0; i < plan.groups->len; i++) {
        MergeGroup *group = g_ptr_array_index(plan.groups, i);
        MergeTU *covering;

        if (!group->tus->len) {
            continue;
        }
        covering = merge_covering_tu(group);
        if (covering) {
            merge_keep_tu(&plan, covering);
        } else {
            if (!merge_translate_group(&plan, group)) {
                fprintf(stderr, "[AOT][Merge] TU group retranslation failed\n");
                goto done;
            }
            retranslated++;
        }
    }
    if (fflush(plan.spool) || !merge_write_plan(&plan, out, mode, pages, tbs)) {
        goto done;
    }
    aot_verbose_log("[AOT][Merge] Retranslated %u overlapping TU groups\n",
            retranslated);
    ok = true;
done:
    if (plan.spool) {
        fclose(plan.spool);
    }
    g_free(plan.copy_buffer);
    g_ptr_array_free(plan.outputs, true);
    g_ptr_array_free(plan.groups, true);
    g_ptr_array_free(plan.tus, true);
    g_hash_table_destroy(plan.pages);
    return ok;
}
#else
bool aot_merge_tu_files(const void *add, uint64_t add_size,
                       const void *base, uint64_t base_size, FILE *out,
                       uint32_t mode, int *pages, int *tbs)
{
    return false;
}
#endif
