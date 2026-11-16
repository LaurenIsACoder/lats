/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SYS_AOT_TU_MERGE_INTERNAL_H
#define SYS_AOT_TU_MERGE_INTERNAL_H

#include "sys-aot-tu-merge.h"

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
    uint64_t entry_pc;
    bool preserve_entry;
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


guint merge_page_hash(gconstpointer p);
gboolean merge_page_equal(gconstpointer ap, gconstpointer bp);
MergePage *merge_mapping_page(const GArray *mappings, uint64_t pc);
void merge_group_free(gpointer opaque);
bool merge_parse_input(MergePlan *plan, const uint8_t *file,
                              uint64_t size);
bool merge_same_tu(const MergeTU *a, const MergeTU *b);
void merge_keep_tu(MergePlan *plan, const MergeTU *tu);
gint merge_mapping_cmp(gconstpointer ap, gconstpointer bp);
bool merge_translate_group(MergePlan *plan, MergeGroup *group);
bool merge_write_plan(MergePlan *plan, FILE *out, uint32_t mode,
                             int *page_count, int *tb_count);

#endif
