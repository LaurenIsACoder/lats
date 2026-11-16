/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qlist.h"
#include "sysemu/sysemu.h"
#include "sysemu/cpus.h"
#include "lsenv.h"
#include "translate.h"
#include "latx-ras.h"
#include "latx-tu.h"
#include "sys-aot.h"
#include "sys-aot-internal.h"
#include "sys-aot-tu-codec.h"
#include "sys-aot-tu-merge-internal.h"
#include "sys-aot-offline.h"

static const char *input_path, *requests_path, *output_path;
static uint64_t owner_id;
static uint32_t raw_mode[3];
static bool retranslate_owner, partition_owner, function_owner, page_source_only;
static bool raw_source, pack_function_source;
static QList *code_page_jobs;
/* The explicit offline mode emits a direct exception without rereading DIV. */
bool aot_offline_div_exception;
unsigned aot_offline_div_exception_translated;

static G_NORETURN void completion_error(const char *message)
{
    fprintf(stderr, "AOT translation failed: %s\n", message);
    fflush(NULL);
    _exit(1);
}

typedef struct FunctionCoverage {
    const MergeOutput *first;
    guint last_tu;
    bool has_nonreturn;
} FunctionCoverage;

static bool function_same_pages(const MergeOutput *first,
        const MergeOutput *current, uint64_t pc, unsigned size)
{
    if (!size || pc > UINT64_MAX - (size - 1)) {
        return false;
    }
    uint64_t last = (pc + size - 1) & TARGET_PAGE_MASK;
    for (pc &= TARGET_PAGE_MASK; ; pc += TARGET_PAGE_SIZE) {
        MergePage *a = NULL, *b = NULL;
        for (uint32_t i = 0; i < first->page_count; i++) {
            if (first->pages[i].pc == pc) {
                a = first->pages[i].page;
            }
        }
        for (uint32_t i = 0; i < current->page_count; i++) {
            if (current->pages[i].pc == pc) {
                b = current->pages[i].page;
            }
        }

        if (!a || !b || !a->bytes || !b->bytes ||
                a->fingerprint != b->fingerprint ||
                memcmp(a->bytes, b->bytes, TARGET_PAGE_SIZE)) {
            return false;
        }
        if (pc == last) {
            return true;
        }
    }
}

static bool function_local_return(const aot_block *blocks, unsigned index,
                                  const uint8_t *code, uint64_t code_size)
{
    if (!index || !latxs_hras_aot_translate()) {
        return false;
    }
    const aot_block *caller = &blocks[index - 1], *next = &blocks[index];
    uint64_t base = caller->tc.offset_in_tu;

    if (!caller->size || !caller->icount || !next->icount || !next->ir2_num ||
            caller->inst_ptr_offset <= 0 ||
            (caller->last_ir1_type != IR1_TYPE_CALL &&
             caller->last_ir1_type != IR1_TYPE_CALLIN) ||
            caller->pc > UINT64_MAX - caller->size ||
            next->pc != caller->pc + caller->size ||
            caller->x86_next_pc != next->pc ||
            caller->flags != next->flags || caller->cflags != next->cflags ||
            caller->cs_base != next->cs_base || base > code_size ||
            (uint64_t)caller->ir2_num * 4 > code_size - base ||
            next->tc.offset_in_tu > code_size ||
            (uint64_t)next->ir2_num * 4 > code_size - next->tc.offset_in_tu) {
        return false;
    }
    if (caller->last_ir1_type == IR1_TYPE_CALL) {
        unsigned width = caller->jmp_reset_offset[0] -
                         caller->jmp_insn_offset[0];
        uint32_t slot[2];

        if (caller->jmp_insn_offset[0] == TB_JMP_OFFSET_INVALID ||
                caller->jmp_reset_offset[0] == TB_JMP_OFFSET_INVALID ||
                (width != 4 && width != 8) ||
                caller->jmp_reset_offset[0] > (uint64_t)caller->ir2_num * 4 ||
                base + caller->jmp_reset_offset[0] != next->tc.offset_in_tu) {
            return false;
        }
        memcpy(slot, code + base + caller->jmp_insn_offset[0], width);
        return (slot[0] & 0xfc000000U) == 0x50000000U &&
               (width == 4 || slot[1] == 0x03400000U);
    }
    uint64_t end = base + (uint64_t)caller->ir2_num * 4;
    uint32_t call;

    if (!caller->ir2_num || end != next->tc.offset_in_tu) {
        return false;
    }
    memcpy(&call, code + end - 4, 4);
    return (call & ~0x3e0U) == 0x4c000001U && (call & 0x3e0U);
}

/* A page-only build must contain only newly translated requested TBs, with
 * exactly the decoded ranges. The first request is the function entry and
 * must remain a TU entry. Never carry source AOT host bytes forward. */
static bool function_outputs_match(MergePlan *plan, const GArray *requests)
{
    FunctionCoverage *seen = g_new0(FunctionCoverage, requests->len);
    size_t matched = 0;
    bool ok = false;
    bool found_entry = false;
    off_t end = ftello(plan->spool);

    if (end < 0 || !plan->outputs->len) {
        goto out;
    }
    for (guint i = 0; i < plan->outputs->len; i++) {
        const MergeOutput *out = g_ptr_array_index(plan->outputs, i);
        uint8_t *bytes;
        if (out->original || out->size < sizeof(aot_tu_head) ||
            fseeko(plan->spool, out->offset, SEEK_SET)) {
            goto out;
        }
        bytes = g_try_malloc(out->size);
        if (!bytes || fread(bytes, 1, out->size, plan->spool) != out->size) {
            g_free(bytes);
            goto out;
        }
        const aot_tu_head *head = (const void *)bytes;
        found_entry |= head->first_pc == g_array_index(requests, aot_block, 0).pc;
        size_t block_start = sizeof(*head) + (size_t)head->page_count * sizeof(aot_tu_page);
        if (head->tu_sum_size != out->size || head->page_count > AOT_TU_MAX_PAGES ||
            !head->tb_count || head->tb_count != out->tb_count ||
            head->page_count != out->page_count || block_start > out->size ||
            head->tb_count > (out->size - block_start) / sizeof(aot_block)) {
            g_free(bytes);
            goto out;
        }
        const aot_block *blocks = (const void *)(bytes + block_start);
        uint64_t code_start = block_start + head->tb_count * sizeof(*blocks);
        uint64_t code_size = blocks[0].tu_size;
        if (!code_size || code_size > out->size - code_start) {
            g_free(bytes);
            goto out;
        }
        const uint8_t *code = bytes + code_start;
        for (uint32_t j = 0; j < head->tb_count; j++) {
            guint k;
            for (k = 0; k < requests->len; k++) {
                const aot_block *want = &g_array_index(requests, aot_block, k);
                if (want->pc == blocks[j].pc) {
                    bool is_return = function_local_return(blocks, j,
                                                            code, code_size);
                    if (want->size != blocks[j].size ||
                        want->icount != blocks[j].icount ||
                        want->flags != blocks[j].flags ||
                        want->cflags != blocks[j].cflags ||
                        want->cs_base != blocks[j].cs_base ||
                        seen[k].last_tu == i + 1 ||
                        (!is_return && seen[k].has_nonreturn) ||
                        !function_same_pages(seen[k].first ? seen[k].first : out,
                                             out, blocks[j].pc, blocks[j].size)) {
                        fprintf(stderr, "FUNCTION_TB_MISMATCH pc=0x%" PRIx64
                                " expected=%u/%u actual=%u/%u flags=%x/%x cflags=%x/%x seen=%d\n",
                                (uint64_t)want->pc, want->size, want->icount,
                                blocks[j].size, blocks[j].icount, want->flags,
                                blocks[j].flags, want->cflags, blocks[j].cflags,
                                seen[k].first != NULL);
                        g_free(bytes);
                        goto out;
                    }
                    if (!seen[k].first) {
                        seen[k].first = out;
                        matched++;
                    }
                    seen[k].last_tu = i + 1;
                    seen[k].has_nonreturn |= !is_return;
                    break;
                }
            }
            if (k == requests->len) {
                fprintf(stderr, "FUNCTION_TB_UNREQUESTED pc=0x%" PRIx64 "\n",
                        (uint64_t)blocks[j].pc);
                g_free(bytes);
                goto out;
            }
        }
        g_free(bytes);
    }
    ok = matched == requests->len && found_entry;
    if (!ok) {
        fprintf(stderr, "FUNCTION_TB_MISSING matched=%zu requested=%u\n",
                matched, requests->len);
    }
out:
    if (end >= 0 && fseeko(plan->spool, end, SEEK_SET)) {
        ok = false;
    }
    g_free(seen);
    return ok;
}

static bool function_pack_outputs(MergePlan *plan, uint64_t entry)
{
    g_autoptr(GPtrArray) fragments =
        g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
    g_autoptr(GBytes) packed = NULL;
    for (guint i = 0; i < plan->outputs->len; i++) {
        const MergeOutput *out = g_ptr_array_index(plan->outputs, i);
        uint8_t *bytes = g_try_malloc(out->size);
        if (!bytes || out->original ||
            fseeko(plan->spool, out->offset, SEEK_SET) ||
            fread(bytes, 1, out->size, plan->spool) != out->size) {
            g_free(bytes);
            return false;
        }
        g_ptr_array_add(fragments, g_bytes_new_take(bytes, out->size));
    }
    packed = aot_pack_function_tus(fragments, entry);
    if (!packed) {
        return false;
    }
    gsize size;
    const aot_tu_head *head = g_bytes_get_data(packed, &size);
    const aot_tu_page *deps = (const void *)(head + 1);
    g_autofree MergeOutput *out = g_new0(MergeOutput, 1);
    out->size = size;
    out->tb_count = head->tb_count;
    out->id = head->tu_id;
    out->anchor = entry & TARGET_PAGE_MASK;
    out->page_count = head->page_count;
    for (uint32_t i = 0; i < head->page_count; i++) {
        uint64_t pc = out->anchor + deps[i].pc_delta;
        MergePage *page = NULL;
        for (guint j = 0; j < plan->outputs->len; j++) {
            const MergeOutput *old = g_ptr_array_index(plan->outputs, j);
            for (uint32_t k = 0; k < old->page_count; k++) {
                if (old->pages[k].pc == pc) {
                    MergePage *found = old->pages[k].page;
                    if (found->fingerprint != deps[i].page_fingerprint ||
                        (page &&
                         memcmp(page->bytes, found->bytes, TARGET_PAGE_SIZE))) {
                        return false;
                    }
                    page = found;
                }
            }
        }
        if (!page) {
            return false;
        }
        out->pages[i] = (MergeMapping){pc, page};
    }
    if (fseeko(plan->spool, 0, SEEK_END) ||
        (out->offset = ftello(plan->spool)) < 0 ||
        fwrite(head, 1, size, plan->spool) != size || fflush(plan->spool)) {
        return false;
    }
    fprintf(stderr, "FUNCTION_SOURCE_PACKED fragments=%u members=%u pages=%u\n",
            fragments->len, out->tb_count, out->page_count);
    g_ptr_array_set_size(plan->outputs, 0);
    g_ptr_array_add(plan->outputs, g_steal_pointer(&out));
    return true;
}

/* Keep the saved copy when retranslation emits the same complete TU. IDs are
 * relative to the anchor page, so require the same absolute anchor as well as
 * matching source pages and every member's execution mode and decoded size. */
static bool function_remove_duplicate_outputs(MergePlan *plan, guint first)
{
    off_t end = ftello(plan->spool);
    bool ok = end >= 0;

    for (guint i = first; ok && i < plan->outputs->len; ) {
        MergeOutput *out = g_ptr_array_index(plan->outputs, i);
        uint8_t *bytes = g_try_malloc(out->size);
        bool duplicate = false;
        if (!bytes || fseeko(plan->spool, out->offset, SEEK_SET) ||
            fread(bytes, 1, out->size, plan->spool) != out->size) {
            g_free(bytes);
            ok = false;
            break;
        }
        if (out->size < sizeof(aot_tu_head)) {
            g_free(bytes);
            ok = false;
            break;
        }
        const aot_tu_head *head = (const void *)bytes;
        size_t start = sizeof(*head) + (size_t)head->page_count * sizeof(aot_tu_page);
        if (head->page_count > AOT_TU_MAX_PAGES || head->page_count != out->page_count ||
            head->tu_sum_size != out->size || start > out->size ||
            head->tb_count > (out->size - start) / sizeof(aot_block)) {
            g_free(bytes);
            ok = false;
            break;
        }
        MergeTU generated = { .head = head, .deps = (const void *)(head + 1),
                              .blocks = (const void *)(bytes + start) };
        for (uint32_t j = 0; j < out->page_count; j++) {
            generated.pages[j] = out->pages[j].page;
        }
        for (guint j = 0; j < plan->tus->len; j++) {
            MergeTU *saved = g_ptr_array_index(plan->tus, j);
            if (saved->head->tu_id != out->id ||
                (saved->head->first_pc & TARGET_PAGE_MASK) != out->anchor) {
                continue;
            }
            /* The replaced owner is not among the retained outputs. */
            if (saved->head->tu_id == owner_id) {
                continue;
            }
            if (duplicate || !merge_same_tu(saved, &generated)) {
                ok = false;
                break;
            }
            duplicate = true;
        }
        g_free(bytes);
        if (ok && duplicate) {
            fprintf(stderr, "FUNCTION_TU_REUSED anchor=0x%" PRIx64
                    " id=0x%" PRIx64 "\n", out->anchor, out->id);
            g_ptr_array_remove_index(plan->outputs, i);
        } else {
            i++;
        }
    }
    return end >= 0 && !fseeko(plan->spool, end, SEEK_SET) && ok;
}

static uint64_t request_number(const char *word, unsigned base)
{
    char *end;
    uint64_t value;

    errno = 0;
    value = g_ascii_strtoull(word, &end, base);
    if (!word[0] || word[0] == '-' || errno || *end) {
        completion_error("invalid request number");
    }
    return value;
}

typedef struct BatchPartitionRequest {
    uint64_t owner_id;
    MergeTU *owner;
    GArray *remaining;
} BatchPartitionRequest;

static bool batch_tu_has_native(const MergeTU *tu)
{
    const uint8_t *code = (const uint8_t *)(tu->blocks + tu->head->tb_count);
    const aot_rel *rels = (const void *)(code + tu->blocks[0].tu_size);

    for (uint32_t i = 0; i < tu->head->rel_count; i++) {
        if (rels[i].kind == AOT_NATIVE_ENTRY || rels[i].kind == AOT_NATIVE_CALL ||
            rels[i].kind == AOT_NATIVE_CORE || rels[i].kind == AOT_NATIVE_CORE_CALL ||
            rels[i].kind == AOT_NATIVE_DIRECT_CALL ||
            rels[i].kind == AOT_NATIVE_LINKED_CALL ||
            rels[i].kind == AOT_NATIVE_INDIRECT_CALL ||
            rels[i].kind == AOT_NATIVE_RET_MISS) {
            return true;
        }
    }
    return false;
}

/* Bound the allocation and read a regular file through one descriptor. A file
 * that grows after fstat is rejected rather than read without a size limit. */
static gchar *batch_read_requests(void)
{
    const size_t limit = 16 * 1024 * 1024;
    struct stat st;
    int fd = open(requests_path, O_RDONLY | O_NONBLOCK);
    size_t at = 0;
    char extra;

    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || st.st_size > limit) {
        completion_error("finite batch request file (maximum 16 MiB)");
    }
    size_t size = st.st_size;
    gchar *requests = g_malloc(size + 1);

    while (at < size) {
        ssize_t n = read(fd, requests + at, size - at);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            completion_error("incomplete batch request file");
        }
        at += n;
    }
    ssize_t n;
    do {
        n = read(fd, &extra, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 0 || memchr(requests, 0, size)) {
        completion_error("batch request file grew or contains NUL");
    }
    close(fd);
    requests[size] = 0;
    return requests;
}

static void batch_validate_owner(const MergeTU *owner)
{
    const aot_block *context = owner->blocks;

    if (batch_tu_has_native(owner)) {
        completion_error("batch source owner contains native records");
    }
    for (uint32_t i = 0; i < owner->head->tb_count; i++) {
        const aot_block *b = &owner->blocks[i];

        if (b->cs_base != context->cs_base || b->flags != context->flags ||
            b->cflags != context->cflags) {
            completion_error("batch partition members differ in execution mode");
        }
    }
    if (context->cs_base || !(context->flags & HF_CS64_MASK) ||
        !(context->flags & HF_HAMT_EN_MASK) ||
        (context->flags & HF_CPL_MASK) != HF_CPL_MASK) {
        completion_error("unsupported batch source execution mode");
    }
}

static GPtrArray *batch_parse_requests(MergePlan *plan, GHashTable *requested)
{
    GHashTable *owners = g_hash_table_new(g_int64_hash, g_int64_equal);
    GPtrArray *result = g_ptr_array_new();
    gchar *text = batch_read_requests();
    gchar **lines = g_strsplit(text, "\n", -1);

    for (guint i = 0; i < plan->tus->len; i++) {
        MergeTU *tu = g_ptr_array_index(plan->tus, i);

        /* NULL records ambiguous input IDs, even if only one is requested. */
        gpointer value = g_hash_table_contains(owners, &tu->head->tu_id) ? NULL : tu;
        g_hash_table_insert(owners, (gpointer)&tu->head->tu_id, value);
    }
    for (guint line = 0; lines[line]; line++) {
        char *row = g_strstrip(lines[line]);
        gchar **words;
        BatchPartitionRequest *request = NULL;
        GHashTable *members = NULL;
        bool *selected = NULL;

        if (!*row) {
            continue;
        }
        words = g_strsplit_set(row, " \t\r", -1);
        for (guint i = 0; words[i]; i++) {
            if (!*words[i]) {
                continue;
            }
            uint64_t value = request_number(words[i], 0);

            if (!request) {
                request = g_new0(BatchPartitionRequest, 1);
                request->owner_id = value;
                request->owner = g_hash_table_lookup(owners, &value);
                if (!request->owner) {
                    completion_error("missing or ambiguous batch source owner");
                }
                if (g_hash_table_contains(requested, &value)) {
                    completion_error("duplicate batch source owner");
                }
                fprintf(stderr, "SAVED_TUS_PARTITION_OWNER id=0x%" PRIx64
                        " original=%u\n", value, request->owner->head->tb_count);
                batch_validate_owner(request->owner);
                request->remaining = g_array_new(false, false, sizeof(aot_block));
                selected = g_new0(bool, request->owner->head->tb_count);
                members = g_hash_table_new(g_int64_hash, g_int64_equal);
                for (uint32_t j = 0; j < request->owner->head->tb_count; j++) {
                    const aot_block *b = &request->owner->blocks[j];

                    if (g_hash_table_contains(members, &b->pc)) {
                        completion_error("ambiguous batch source member");
                    }
                    g_hash_table_insert(members, (gpointer)&b->pc, (gpointer)b);
                }
            } else {
                const aot_block *b = g_hash_table_lookup(members, &value);

                if (!b) {
                    completion_error("batch remaining member does not belong to owner");
                }
                size_t index = b - request->owner->blocks;
                if (selected[index]) {
                    completion_error("duplicate batch remaining member");
                }
                selected[index] = true;
            }
        }
        g_assert(request);
        for (uint32_t i = 0; i < request->owner->head->tb_count; i++) {
            if (selected[i]) {
                g_array_append_val(request->remaining, request->owner->blocks[i]);
            }
        }
        if (request->remaining->len >= request->owner->head->tb_count) {
            completion_error("batch remaining set must remove at least one member");
        }
        fprintf(stderr, "SAVED_TUS_PARTITION_REQUEST id=0x%" PRIx64
                " remaining=%u removed=%u\n", request->owner_id,
                request->remaining->len,
                request->owner->head->tb_count - request->remaining->len);
        g_hash_table_insert(requested, &request->owner_id, request);
        g_ptr_array_add(result, request);
        g_hash_table_destroy(members);
        g_free(selected);
        g_strfreev(words);
    }
    g_strfreev(lines);
    g_free(text);
    g_hash_table_destroy(owners);
    if (!result->len) {
        completion_error("empty batch request");
    }
    fprintf(stderr, "SAVED_TUS_PARTITION_REQUESTS %u\n", result->len);
    return result;
}

/* Build a comparison view over either a preserved record or spool output.
 * The caller owns storage and restores the append position after comparison. */
static bool batch_output_view(MergePlan *plan, const MergeOutput *out,
                              MergeTU *tu, uint8_t **storage)
{
    const uint8_t *bytes = out->original;

    *storage = NULL;
    if (!bytes) {
        *storage = g_try_malloc(out->size);
        if (!*storage || fseeko(plan->spool, out->offset, SEEK_SET) ||
            fread(*storage, 1, out->size, plan->spool) != out->size) {
            return false;
        }
        bytes = *storage;
    }
    if (out->size < sizeof(aot_tu_head)) {
        return false;
    }
    const aot_tu_head *head = (const void *)bytes;
    size_t start = sizeof(*head) + (size_t)head->page_count * sizeof(aot_tu_page);

    if (!head->tb_count || !head->page_count ||
        head->page_count > AOT_TU_MAX_PAGES ||
        head->page_count != out->page_count || head->tb_count != out->tb_count ||
        head->tu_id != out->id || head->tu_sum_size != out->size ||
        (head->first_pc & TARGET_PAGE_MASK) != out->anchor ||
        start > out->size ||
        head->tb_count > (out->size - start) / sizeof(aot_block)) {
        return false;
    }
    *tu = (MergeTU){ .head = head, .deps = (const void *)(head + 1),
                    .blocks = (const void *)(bytes + start) };
    size_t code = start + (size_t)head->tb_count * sizeof(aot_block);

    if (tu->blocks[0].tu_size < 0 ||
        tu->blocks[0].tu_size > out->size - code ||
        head->rel_count > (out->size - code - tu->blocks[0].tu_size) / sizeof(aot_rel)) {
        return false;
    }
    for (uint32_t i = 0; i < out->page_count; i++) {
        tu->pages[i] = out->pages[i].page;
        if (out->pages[i].pc != out->anchor + tu->deps[i].pc_delta ||
            tu->pages[i]->fingerprint != tu->deps[i].page_fingerprint) {
            return false;
        }
    }
    return true;
}

static void batch_index_output(GHashTable *identities, MergeOutput *out)
{
    GPtrArray *same = g_hash_table_lookup(identities, &out->id);

    if (!same) {
        same = g_ptr_array_new();
        g_hash_table_insert(identities, &out->id, same);
    }
    g_ptr_array_add(same, out);
}

/* Compare only outputs that are actually retained. Replaced owners are absent
 * from this index. Include prior generated outputs, including this same group. */
static bool batch_remove_duplicate_outputs(MergePlan *plan, guint first,
                                           GHashTable *identities)
{
    off_t end = ftello(plan->spool);
    bool ok = end >= 0;

    for (guint i = first; ok && i < plan->outputs->len; ) {
        MergeOutput *out = g_ptr_array_index(plan->outputs, i);
        MergeTU generated;
        uint8_t *generated_bytes;
        bool duplicate = false;

        if (!batch_output_view(plan, out, &generated, &generated_bytes)) {
            g_free(generated_bytes);
            ok = false;
            break;
        }
        if (batch_tu_has_native(&generated)) {
            g_free(generated_bytes);
            ok = false;
            break;
        }
        GPtrArray *same = g_hash_table_lookup(identities, &out->id);

        for (guint j = 0; same && ok && j < same->len; j++) {
            MergeOutput *previous = g_ptr_array_index(same, j);
            MergeTU saved;
            uint8_t *saved_bytes;

            if (!batch_output_view(plan, previous, &saved, &saved_bytes)) {
                g_free(saved_bytes);
                ok = false;
                break;
            }
            if (!merge_same_tu(&saved, &generated)) {
                fprintf(stderr, "SAVED_TUS_PARTITION_ID_CONFLICT id=0x%" PRIx64
                        " anchors=0x%" PRIx64 "/0x%" PRIx64 "\n",
                        out->id, previous->anchor, out->anchor);
                ok = false;
            } else if (previous->anchor == out->anchor) {
                if (batch_tu_has_native(&saved)) {
                    fprintf(stderr, "SAVED_TUS_PARTITION_NATIVE_CONFLICT id=0x%"
                            PRIx64 " anchor=0x%" PRIx64 "\n", out->id, out->anchor);
                    ok = false;
                } else {
                    duplicate = true;
                }
            } else if (previous->page_count > 1 || out->page_count > 1) {
                /* The on-disk cross-page references are keyed by ID/delta;
                 * distinct absolute anchors cannot share that namespace. */
                fprintf(stderr, "SAVED_TUS_PARTITION_CROSS_PAGE_ID_CONFLICT id=0x%"
                        PRIx64 " anchors=0x%" PRIx64 "/0x%" PRIx64 "\n",
                        out->id, previous->anchor, out->anchor);
                ok = false;
            }
            g_free(saved_bytes);
        }
        g_free(generated_bytes);
        if (ok && duplicate) {
            fprintf(stderr, "SAVED_TUS_PARTITION_TU_REUSED anchor=0x%" PRIx64
                    " id=0x%" PRIx64 "\n", out->anchor, out->id);
            g_ptr_array_remove_index(plan->outputs, i);
        } else if (ok) {
            batch_index_output(identities, out);
            i++;
        }
    }
    return end >= 0 && !fseeko(plan->spool, end, SEEK_SET) && ok;
}

static void partition_saved_tus(CPUState *cpu, run_on_cpu_data unused)
{
    MergePlan plan = {
        .pages = g_hash_table_new_full(merge_page_hash, merge_page_equal,
                                     g_free, NULL),
        .tus = g_ptr_array_new_with_free_func(g_free),
        .groups = g_ptr_array_new_with_free_func(merge_group_free),
        .outputs = g_ptr_array_new_with_free_func(g_free),
    };
    GHashTable *requested = g_hash_table_new(g_int64_hash, g_int64_equal);
    GHashTable *identities = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                           NULL, (GDestroyNotify)g_ptr_array_unref);
    gchar *data, *generated;
    gsize size, generated_size;
    gchar *spool_path = g_strconcat(output_path, ".spool", NULL);
    uint64_t requested_members = 0;
    int fd, pages, tbs;

    offline_cpu = aot_tu_merge_cpu = cpu;
    in_aot_translate = 1;
    option_sys_aot = option_aot_save = 1;
    option_tu_mode = LATX_TU_MODE_AOT;
    option_aot_cross_page_tu = 1;
    aot_reloc_init();
    fd = open(input_path, O_RDONLY);
    if (fd < 0 || !test_integrity(fd, AOT_USER) ||
        !g_file_get_contents(input_path, &data, &size, NULL) ||
        !validate_aot_layout(data, size)) {
        completion_error("batch input AOT identity or layout");
    }
    close(fd);
    if (!merge_parse_input(&plan, (uint8_t *)data, size)) {
        completion_error("batch source AOT dependency records");
    }
    GPtrArray *requests = batch_parse_requests(&plan, requested);

    /* Copy each unaffected TU exactly once, including every native owner. */
    for (guint i = 0; i < plan.tus->len; i++) {
        MergeTU *tu = g_ptr_array_index(plan.tus, i);

        if (!g_hash_table_contains(requested, &tu->head->tu_id)) {
            merge_keep_tu(&plan, tu);
            batch_index_output(identities,
                               g_ptr_array_index(plan.outputs, plan.outputs->len - 1));
        }
    }
    /* A named spool survives a failed translation or verification. */
    fd = open(spool_path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || !(plan.spool = fdopen(fd, "w+b"))) {
        completion_error("create exclusive batch spool");
    }
    fprintf(stderr, "SAVED_TUS_PARTITION_SPOOL %s\n", spool_path);
    for (guint i = 0; i < requests->len; i++) {
        BatchPartitionRequest *request = g_ptr_array_index(requests, i);
        MergeTU *owner = request->owner;
        const aot_block *context = owner->blocks;
        CPUX86State *env = cpu->env_ptr;
        guint first = plan.outputs->len;

        requested_members += request->remaining->len;
        if (request->remaining->len) {
            env->hflags = context->flags;
            env->cr[0] = CR0_PE_MASK | CR0_ET_MASK | CR0_PG_MASK | CR0_WP_MASK;
            env->cr[4] = CR4_PAE_MASK | CR4_PSE_MASK;
            env->efer = MSR_EFER_LME | MSR_EFER_LMA | MSR_EFER_NXE;
            if (!can_do_hamt(env)) {
                completion_error("requested batch HAMT mode is not active");
            }
            aot_tu_head head = { .tb_count = request->remaining->len };
            MergeGroup group = { .tus = g_ptr_array_new(),
                                .mappings = owner->group->mappings };
            MergeTU synthetic = { .head = &head,
                                  .blocks = (aot_block *)request->remaining->data,
                                  .bias = owner->bias, .group = &group };

            g_ptr_array_add(group.tus, &synthetic);
            if (!merge_translate_group(&plan, &group) || fflush(plan.spool)) {
                completion_error("batch ordinary file-backed translation");
            }
            g_ptr_array_free(group.tus, true);
            if (!batch_remove_duplicate_outputs(&plan, first, identities)) {
                completion_error("batch generated TU conflicts with retained output");
            }
        }
        fprintf(stderr, "SAVED_TUS_PARTITION_OWNER_DONE id=0x%" PRIx64
                " remaining=%u generated=%u total_outputs=%u\n",
                request->owner_id, request->remaining->len,
                plan.outputs->len - first, plan.outputs->len);
    }
    fd = open(output_path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || !(out_tb_file = fdopen(fd, "w+b")) ||
        !merge_write_plan(&plan, out_tb_file, AOT_USER, &pages, &tbs) ||
        fflush(out_tb_file) || !test_integrity(fd, AOT_USER) ||
        !g_file_get_contents(output_path, &generated, &generated_size, NULL) ||
        !validate_aot_layout(generated, generated_size)) {
        completion_error("batch output write, identity or layout");
    }
    if (fclose(out_tb_file) || fclose(plan.spool)) {
        completion_error("close batch output or spool");
    }
    out_tb_file = NULL;
    plan.spool = NULL;
    /* The host still has to prove exact members/pages and publish its result.
     * Its success cleanup removes this spool together with the job directory. */
    fprintf(stderr, "SAVED_TUS_PARTITION_GENERATED requests=%u remaining=%" PRIu64
            " outputs=%u pages=%d total_tbs=%d\n", requests->len,
            requested_members, plan.outputs->len, pages, tbs);
    fflush(NULL);
    _exit(0);
}

static void translate_blocks(CPUState *cpu)
{
    MergePlan plan = {
        .pages = g_hash_table_new_full(merge_page_hash, merge_page_equal,
                                     g_free, NULL),
        .tus = g_ptr_array_new_with_free_func(g_free),
        .groups = g_ptr_array_new_with_free_func(merge_group_free),
        .outputs = g_ptr_array_new_with_free_func(g_free),
        .spool = tmpfile(),
    };
    gchar *data, *requests, *generated;
    gsize size, requests_size, generated_size;
    MergeTU *owner = NULL;
    GArray *blocks = g_array_new(false, true, sizeof(aot_block));
    int fd, pages, tbs;

    if (!plan.spool) {
        completion_error("create translation spool");
    }
    if (raw_source) {
        if (!g_file_get_contents(input_path, &data, &size, NULL) || size < 8 ||
            memcmp(data, "LTPAGES1", 8) || (size - 8) % 4104 ||
            !(size - 8) || (size - 8) / 4104 > AOT_TU_MAX_PAGES) {
            completion_error("invalid code pages or execution mode");
        }
        MergeGroup *group = g_new0(MergeGroup, 1);
        group->tus = g_ptr_array_new();
        group->mappings = g_array_new(false, false, sizeof(MergeMapping));
        group->page_index = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                                 g_free, g_free);
        group->member_index = g_hash_table_new(g_int64_hash, g_int64_equal);
        for (size_t at = 8; at < size; at += 4104) {
            uint64_t pc;
            memcpy(&pc, data + at, 8);
            if ((pc & ~TARGET_PAGE_MASK) || pc > UINT64_MAX - 4096 ||
                g_hash_table_contains(group->page_index, &pc)) {
                completion_error("invalid or duplicate code page");
            }
            MergePage *page = g_new0(MergePage, 1);
            page->bytes = (const uint8_t *)data + at + 8;
            page->hash = get_page_hash((uint32_t *)page->bytes);
            page->fingerprint = get_page_fingerprint(page->bytes);
            MergeMapping mapping = { pc, page };
            g_array_append_val(group->mappings, mapping);
            g_hash_table_insert(group->page_index, g_memdup2(&pc, sizeof(pc)), page);
        }
        aot_tu_head *head = g_new0(aot_tu_head, 1);
        aot_block *context = g_new0(aot_block, 1);
        head->tb_count = 1;
        head->first_pc = g_array_index(group->mappings, MergeMapping, 0).pc;
        context->pc = head->first_pc;
        context->cs_base = raw_mode[0];
        context->flags = raw_mode[1];
        context->cflags = raw_mode[2];
        owner = g_new0(MergeTU, 1);
        owner->head = head;
        owner->blocks = context;
        owner->group = group;
        g_ptr_array_add(group->tus, owner);
        g_ptr_array_add(plan.tus, owner);
        g_ptr_array_add(plan.groups, group);
    } else {
    fd = open(input_path, O_RDONLY);
    if (fd < 0 || (!page_source_only && !test_integrity(fd, AOT_USER)) ||
        !g_file_get_contents(input_path, &data, &size, NULL) ||
        !validate_aot_layout(data, size)) {
        completion_error("input AOT identity or layout");
    }
    if (page_source_only) {
        const aot_tail *tail = (const void *)(data + size - sizeof(*tail));
        if (!memchr(tail->aot_version, '\0', sizeof(tail->aot_version)) ||
            strcmp(tail->aot_version, AOT_VERSION) ||
            !(tail->flags & (UINT64_C(1) << AOT_TU_FLAG_BIT))) {
            completion_error("source page AOT format version");
        }
    }
    if (!merge_parse_input(&plan, (uint8_t *)data, size)) {
        completion_error("source page AOT dependency records");
    }
    close(fd);
    for (guint i = 0; i < plan.tus->len; i++) {
        MergeTU *tu = g_ptr_array_index(plan.tus, i);

        if (tu->head->tu_id == owner_id) {
            if (owner) {
                completion_error("ambiguous source owner");
            }
            owner = tu;
        }
        if (!page_source_only && (!retranslate_owner || tu->head->tu_id != owner_id)) {
            merge_keep_tu(&plan, tu);
        }
    }
    if (!owner) {
        completion_error("missing source owner");
    }
    }
    if (retranslate_owner && !partition_owner && !function_owner) {
        requests = g_strdup("");
    } else if (!g_file_get_contents(requests_path, &requests,
                                     &requests_size, NULL) ||
        requests_size > 64 * 1024 || memchr(requests, 0, requests_size)) {
        completion_error("finite request file");
    }
    const aot_block *context = owner->blocks;
    CPUX86State *env = cpu->env_ptr;

    if (partition_owner || function_owner) {
        const uint8_t *code = (const uint8_t *)(owner->blocks + owner->head->tb_count);
        const aot_rel *rels = (const void *)(code + context->tu_size);

        for (uint32_t i = 0; !page_source_only && i < owner->head->rel_count; i++) {
            if (rels[i].kind == AOT_NATIVE_ENTRY || rels[i].kind == AOT_NATIVE_CALL ||
                rels[i].kind == AOT_NATIVE_CORE || rels[i].kind == AOT_NATIVE_CORE_CALL ||
                rels[i].kind == AOT_NATIVE_DIRECT_CALL ||
                rels[i].kind == AOT_NATIVE_LINKED_CALL ||
                rels[i].kind == AOT_NATIVE_INDIRECT_CALL) {
                completion_error("source owner contains native records");
            }
        }
        for (uint32_t i = 0; i < owner->head->tb_count; i++) {
            const aot_block *b = &owner->blocks[i];
            if (b->cs_base != context->cs_base || b->flags != context->flags ||
                b->cflags != context->cflags) {
                completion_error("partition members differ in execution mode");
            }
        }
    }

    g_array_sort(owner->group->mappings, merge_mapping_cmp);

    /* Ordinary templates use the 64-bit user CPL and HAMT execution mode. */
    if (context->cs_base || !(context->flags & HF_CS64_MASK) ||
        !(context->flags & HF_HAMT_EN_MASK) ||
        (context->flags & HF_CPL_MASK) != HF_CPL_MASK) {
        completion_error("unsupported source execution mode");
    }
    env->hflags = context->flags;
    env->cr[0] = CR0_PE_MASK | CR0_ET_MASK | CR0_PG_MASK | CR0_WP_MASK;
    env->cr[4] = CR4_PAE_MASK | CR4_PSE_MASK;
    env->efer = MSR_EFER_LME | MSR_EFER_LMA | MSR_EFER_NXE;
    if (!can_do_hamt(env)) {
        completion_error("requested HAMT mode is not active");
    }
    gchar **lines = g_strsplit(requests, "\n", -1);

    for (guint line = 0; lines[line]; line++) {
        char *text = g_strstrip(lines[line]);
        gchar **words;
        const char *tokens[3];
        unsigned count = 0;

        if (!*text) {
            continue;
        }
        words = g_strsplit_set(text, " \t", -1);
        for (unsigned j = 0; words[j]; j++) {
            if (*words[j]) {
                if (count == G_N_ELEMENTS(tokens)) {
                    completion_error("request must contain PC, bytes, icount");
                }
                tokens[count++] = words[j];
            }
        }
        if (partition_owner) {
            if (count != 1 || blocks->len >= owner->head->tb_count) {
                completion_error("partition request must contain one member PC");
            }
            uint64_t pc = request_number(tokens[0], 0);
            const aot_block *member = NULL;

            for (uint32_t i = 0; i < owner->head->tb_count; i++) {
                if (owner->blocks[i].pc == pc) {
                    if (member) {
                        completion_error("ambiguous partition member");
                    }
                    member = &owner->blocks[i];
                }
            }
            if (!member || member == owner->blocks) {
                completion_error("partition member missing or is owner root");
            }
            for (guint i = 0; i < blocks->len; i++) {
                if (g_array_index(blocks, aot_block, i).pc == pc) {
                    completion_error("duplicate partition member");
                }
            }
            g_array_append_val(blocks, *member);
            g_strfreev(words);
            continue;
        }
        unsigned request_limit = function_owner ? INT_MAX : MAX_TB_IN_AOT_TU;
        if (count != 3 || blocks->len >= request_limit) {
            completion_error("invalid or excessive requests");
        }
        uint64_t pc = request_number(tokens[0], 0);
        uint64_t bytes = request_number(tokens[1], 10);
        uint64_t icount = request_number(tokens[2], 10);
        if (aot_offline_div_exception && icount != 1) {
            completion_error("exception stub requires a single guest instruction");
        }
        aot_block block = { .pc = pc, .cs_base = context->cs_base,
            .flags = context->flags, .cflags = context->cflags };

        if (!bytes || bytes > UINT16_MAX || !icount ||
            icount > UINT16_MAX || pc > UINT64_MAX - bytes ||
            !merge_mapping_page(owner->group->mappings, pc & TARGET_PAGE_MASK) ||
            !merge_mapping_page(owner->group->mappings,
                                (pc + bytes - 1) & TARGET_PAGE_MASK) ||
            bytes > TARGET_PAGE_SIZE) {
            completion_error("request outside saved owner pages or TB limits");
        }
        for (guint i = 0; !function_owner && i < plan.tus->len; i++) {
            MergeTU *tu = g_ptr_array_index(plan.tus, i);
            bool same_source = true;

            for (guint j = 0; j < tu->group->mappings->len; j++) {
                MergeMapping m = g_array_index(tu->group->mappings,
                                               MergeMapping, j);

                if (merge_mapping_page(owner->group->mappings, m.pc) != m.page) {
                    same_source = false;
                    break;
                }
            }
            if (!same_source) {
                continue;
            }

            for (uint32_t j = 0; j < tu->head->tb_count; j++) {
                const aot_block *b = &tu->blocks[j];

                if (b->pc == pc && b->cs_base == block.cs_base &&
                    b->flags == block.flags && b->cflags == block.cflags) {
                    completion_error("requested member already exists");
                }
            }
        }
        for (guint i = 0; i < blocks->len; i++) {
            if (g_array_index(blocks, aot_block, i).pc == pc) {
                completion_error("duplicate request");
            }
        }
        block.size = bytes;
        block.icount = icount;
        g_array_append_val(blocks, block);
        g_strfreev(words);
    }
    if (!blocks->len && (!retranslate_owner || partition_owner || function_owner)) {
        completion_error("empty request");
    }
    if (function_owner && !page_source_only) {
        bool found_root = false;
        for (guint i = 0; i < blocks->len; i++) {
            found_root |= g_array_index(blocks, aot_block, i).pc == context->pc;
        }
        if (!found_root) {
            completion_error("function requests omit owner entry");
        }
    }
    aot_tu_head head = { .tb_count = blocks->len };
    MergeTU synthetic = retranslate_owner && !function_owner ? *owner :
        (MergeTU){ .head = &head, .blocks = (aot_block *)blocks->data };
    MergeGroup group = { .tus = g_ptr_array_new(),
                        .mappings = owner->group->mappings,
                        .entry_pc = function_owner && blocks->len ?
                            (page_source_only ? g_array_index(blocks, aot_block, 0).pc :
                                                context->pc) : 0,
                        .preserve_entry = function_owner && blocks->len };

    synthetic.group = &group;
    bool translated;
    guint previous_outputs = plan.outputs->len;
    if (partition_owner) {
        GArray *remaining = g_array_new(false, true, sizeof(aot_block));

        for (uint32_t i = 0; i < owner->head->tb_count; i++) {
            const aot_block *member = &owner->blocks[i];
            bool selected = false;

            for (guint j = 0; j < blocks->len; j++) {
                if (g_array_index(blocks, aot_block, j).pc == member->pc) {
                    selected = true;
                    break;
                }
            }
            if (!selected) {
                g_array_append_val(remaining, *member);
            }
        }
        aot_tu_head remaining_head = { .tb_count = remaining->len };
        MergeTU remainder = { .head = &remaining_head,
                              .blocks = (aot_block *)remaining->data,
                              .bias = owner->bias, .group = &group };
        synthetic.head = &head;
        synthetic.blocks = (aot_block *)blocks->data;
        g_ptr_array_add(group.tus, &remainder);
        translated = merge_translate_group(&plan, &group);
        g_ptr_array_set_size(group.tus, 0);
        g_ptr_array_add(group.tus, &synthetic);
        translated = translated && merge_translate_group(&plan, &group);
        g_array_free(remaining, true);
    } else {
        g_ptr_array_add(group.tus, &synthetic);
        translated = merge_translate_group(&plan, &group);
    }
    if (!translated || fflush(plan.spool)) {
        completion_error("ordinary file-backed translation");
    }
    if (function_owner && !page_source_only &&
        !function_remove_duplicate_outputs(&plan, previous_outputs)) {
        completion_error("duplicate function TU differs from saved TU");
    }
    if (function_owner) {
        for (guint i = previous_outputs; i < plan.outputs->len; i++) {
            const MergeOutput *tu = g_ptr_array_index(plan.outputs, i);
            fprintf(stderr, "FUNCTION_TU_GROUP anchor=0x%" PRIx64
                    " members=%u pages=%u id=0x%" PRIx64 "\n",
                    tu->anchor, tu->tb_count, tu->page_count, tu->id);
        }
    }
    if (aot_offline_div_exception && aot_offline_div_exception_translated != blocks->len) {
        completion_error("exception stub translation did not cover every request");
    }
    if (page_source_only && (previous_outputs || !function_outputs_match(&plan, blocks))) {
        completion_error("source-page output differs from requested guest TBs");
    }
    if (pack_function_source &&
        !function_pack_outputs(&plan, g_array_index(blocks, aot_block, 0).pc)) {
        completion_error("whole-function source packing");
    }
    fd = open(output_path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || !(out_tb_file = fdopen(fd, "w+b")) ||
        !merge_write_plan(&plan, out_tb_file, AOT_USER, &pages, &tbs) ||
        fflush(out_tb_file) || !test_integrity(fd, AOT_USER) ||
        !g_file_get_contents(output_path, &generated, &generated_size, NULL) ||
        !validate_aot_layout(generated, generated_size)) {
        completion_error("output write, identity or layout");
    }
    if (fclose(out_tb_file)) {
        completion_error("close translation output");
    }
    out_tb_file = NULL;
    fprintf(stderr, "SAVED_TB_COMPLETION_GENERATED %u %s, %d total TBs\n",
            retranslate_owner && !function_owner ? owner->head->tb_count : blocks->len,
            retranslate_owner ? "retranslated members" : "requests", tbs);
    /* The caller verifies guest extents, CALL sites and resume entries. */
    g_ptr_array_free(group.tus, true);
    g_strfreev(lines);
    g_free(requests);
    g_free(generated);
    g_array_free(blocks, true);
    if (raw_source) {
        g_free((void *)owner->head);
        g_free((void *)owner->blocks);
    }
    g_ptr_array_free(plan.outputs, true);
    g_ptr_array_free(plan.tus, true);
    g_ptr_array_free(plan.groups, true);
    g_hash_table_destroy(plan.pages);
    g_free(plan.copy_buffer);
    if (fclose(plan.spool)) {
        completion_error("close translation spool");
    }
    g_free(data);
}

static void prepare_offline_cpu(CPUState *cpu)
{
    offline_cpu = aot_tu_merge_cpu = cpu;
    in_aot_translate = 1;
    option_sys_aot = option_aot_save = 1;
    option_tu_mode = LATX_TU_MODE_AOT;
    option_aot_cross_page_tu = 1;
    aot_reloc_init();
}

static void complete_blocks(CPUState *cpu, run_on_cpu_data unused)
{
    prepare_offline_cpu(cpu);
    translate_blocks(cpu);
    fflush(NULL);
    _exit(0);
}

static void parse_code_page_mode(const char *mode)
{
    g_auto(GStrv) words = g_strsplit(mode, ":", -1);

    if (g_strv_length(words) != 3) {
        completion_error("mode must contain CS_BASE:FLAGS:CFLAGS");
    }
    for (unsigned i = 0; i < 3; i++) {
        uint64_t value = request_number(words[i], 0);

        if (value > UINT32_MAX) {
            completion_error("execution mode must fit three uint32 fields");
        }
        raw_mode[i] = value;
    }
}

static void select_code_page_job(const QDict *job)
{
    const char *operation = job ? qdict_get_try_str(job, "operation") : NULL;
    const char *mode = job ? qdict_get_try_str(job, "mode") : NULL;

    input_path = job ? qdict_get_try_str(job, "pages") : NULL;
    requests_path = job ? qdict_get_try_str(job, "requests") : NULL;
    output_path = job ? qdict_get_try_str(job, "output") : NULL;
    if (!operation || !mode || !input_path || !*input_path ||
        !requests_path || !*requests_path || !output_path || !*output_path ||
        (strcmp(operation, "ordinary") && strcmp(operation, "function") &&
         strcmp(operation, "division"))) {
        completion_error("invalid code-page batch job");
    }
    raw_source = page_source_only = function_owner = retranslate_owner = true;
    partition_owner = false;
    pack_function_source = !strcmp(operation, "function");
    aot_offline_div_exception = !strcmp(operation, "division");
    aot_offline_div_exception_translated = 0;
    parse_code_page_mode(mode);
}

static void complete_code_page_batch(CPUState *cpu, run_on_cpu_data unused)
{
    const QListEntry *entry;
    unsigned index = 0;

    prepare_offline_cpu(cpu);
    QLIST_FOREACH_ENTRY(code_page_jobs, entry) {
        fprintf(stderr, "CODE_PAGE_BATCH_BEGIN %u\n", index);
        fflush(stderr);
        select_code_page_job(qobject_to(QDict, qlist_entry_obj(entry)));
        translate_blocks(cpu);
        /* Only closed and validated output files receive a completion record.
         * Stop on failure; the caller can restart the unfinished jobs safely. */
        fprintf(stderr, "CODE_PAGE_BATCH_DONE %u\n", index++);
        fflush(NULL);
    }
    qobject_unref(code_page_jobs);
    _exit(0);
}

static void run_cow_job_bucket(CPUState *cpu, GPtrArray *bucket)
{
    if (!getenv("LATS_LLVM_AOT_KEEP_AFFINITY")) {
        cpu_set_t all_cpus;
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        CPU_ZERO(&all_cpus);
        for (long c = 0; c < nprocs && c < CPU_SETSIZE; c++) {
            CPU_SET((int)c, &all_cpus);
        }
        if (nprocs > 0) {
            sched_setaffinity(0, sizeof(all_cpus), &all_cpus);
        }
    }
    uint32_t *cur = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (cur == MAP_FAILED) {
        _exit(1);
    }
    guint start = 0;
    while (start < bucket->len) {
        *cur = start;
        fflush(NULL);
        pid_t runner = fork();
        if (runner < 0) {
            munmap(cur, sizeof(uint32_t));
            _exit(1);
        }
        if (runner == 0) {
            sigset_t empty;
            sigemptyset(&empty);
            sigprocmask(SIG_SETMASK, &empty, NULL);
            CPUX86State *env = cpu->env_ptr;
            current_cpu = cpu;
            if (lsenv) {
                lsenv->cpu_state = env;
                if (lsenv->tr_data) {
                    lsenv->tr_data->inst_do_opt_mmio = 0;
                }
            }
            env->eip = 0;
            out_tb_file = NULL;
            prepare_offline_cpu(cpu);
            for (guint k = start; k < bucket->len; k++) {
                *cur = k;
                const QDict *job = g_ptr_array_index(bucket, k);
                select_code_page_job(job);
                translate_blocks(cpu);
                *cur = k + 1;
            }
            fflush(NULL);
            _exit(0);
        }
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(runner, &status, 0);
        } while (waited < 0 && errno == EINTR);
        uint32_t progressed = *cur;
        if (waited == runner && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0 && progressed == bucket->len) {
            break;
        }
        guint failed_idx = progressed < bucket->len ? progressed : start;
        const QDict *failed_job = g_ptr_array_index(bucket, failed_idx);
        const char *failed_src = qdict_get_try_str(failed_job, "source");
        const char *failed_out = qdict_get_try_str(failed_job, "output");
        if (failed_out && *failed_out) {
            unlink(failed_out);
        }
        start = failed_idx + 1;
        while (start < bucket->len && failed_src) {
            const QDict *next_job = g_ptr_array_index(bucket, start);
            const char *next_src = qdict_get_try_str(next_job, "source");
            if (!next_src || strcmp(next_src, failed_src) != 0) {
                break;
            }
            const char *next_out = qdict_get_try_str(next_job, "output");
            if (next_out && *next_out) {
                unlink(next_out);
            }
            start++;
        }
    }
    munmap(cur, sizeof(uint32_t));
    _exit(0);
}

bool aot_offline_run_batch_cow(struct CPUState *cpu, const char *jobs_json_path,
                               unsigned workers)
{
    g_autofree gchar *data = NULL;
    gsize size = 0;

    if (!cpu || !jobs_json_path ||
        !g_file_get_contents(jobs_json_path, &data, &size, NULL) ||
        memchr(data, 0, size)) {
        return false;
    }
    QObject *parsed = qobject_from_json(data, NULL);
    QList *jobs = qobject_to(QList, parsed);
    if (!jobs) {
        qobject_unref(parsed);
        return false;
    }
    if (qlist_empty(jobs)) {
        qobject_unref(jobs);
        return true;
    }
    if (workers < 1) {
        workers = 1;
    } else if (workers > 8) {
        workers = 8;
    }
    GPtrArray *buckets[8] = { NULL };
    for (unsigned w = 0; w < workers; w++) {
        buckets[w] = g_ptr_array_new();
    }
    GHashTable *source_bucket =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    unsigned next_bucket = 0;
    const QListEntry *entry;
    QLIST_FOREACH_ENTRY(jobs, entry) {
        QDict *job = qobject_to(QDict, qlist_entry_obj(entry));
        if (!job) {
            continue;
        }
        const char *src = qdict_get_try_str(job, "source");
        unsigned b = 0;
        gpointer found = NULL;
        if (src && g_hash_table_lookup_extended(source_bucket, src, NULL, &found)) {
            b = GPOINTER_TO_UINT(found);
        } else {
            b = next_bucket % workers;
            next_bucket++;
            if (src) {
                g_hash_table_insert(source_bucket, g_strdup(src), GUINT_TO_POINTER(b));
            }
        }
        g_ptr_array_add(buckets[b], job);
    }
    g_hash_table_destroy(source_bucket);

    pid_t pids[8];
    unsigned spawned = 0;
    bool ok = true;
    fflush(NULL);
    for (unsigned w = 0; w < workers; w++) {
        if (!buckets[w]->len) {
            pids[w] = -1;
            continue;
        }
        pid_t pid = fork();
        if (pid < 0) {
            pids[w] = -1;
            ok = false;
            continue;
        }
        if (pid == 0) {
            run_cow_job_bucket(cpu, buckets[w]);
        }
        pids[w] = pid;
        spawned++;
    }
    for (unsigned w = 0; w < workers; w++) {
        if (pids[w] > 0) {
            int status = 0;
            pid_t waited;
            do {
                waited = waitpid(pids[w], &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited != pids[w] || !WIFEXITED(status) ||
                WEXITSTATUS(status) != 0) {
                ok = false;
            }
        }
        g_ptr_array_free(buckets[w], true);
    }
    qobject_unref(jobs);
    return ok && spawned > 0;
}

int aot_offline_main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--compile-code-pages-batch")) {
        g_autofree gchar *data = NULL;
        gsize size;

        if (argc < 4 || !g_file_get_contents(argv[2], &data, &size, NULL) ||
            memchr(data, 0, size)) {
            completion_error("usage: --compile-code-pages-batch JOBS.json, followed by QEMU options");
        }
        QObject *jobs = qobject_from_json(data, NULL);

        code_page_jobs = qobject_to(QList, jobs);
        if (!code_page_jobs || qlist_empty(code_page_jobs)) {
            completion_error("code-page batch must be a nonempty JSON array");
        }
        for (int i = 3; i <= argc; i++) {
            argv[i - 2] = argv[i];
        }
        qemu_init(argc - 2, argv);
        if (!first_cpu) {
            completion_error("missing vCPU");
        }
        async_safe_run_on_cpu(first_cpu, complete_code_page_batch, RUN_ON_CPU_NULL);
        qemu_main_loop();
        completion_error("vCPU code-page batch callback did not finish");
    }
    if (argc == 3 && !strcmp(argv[1], "--validate-saved-aot")) {
        gchar *data;
        gsize size;
        if (!g_file_get_contents(argv[2], &data, &size, NULL) ||
            !validate_aot_layout(data, size)) {
            completion_error("saved AOT layout");
        }
        g_free(data);
        puts("SAVED_AOT_LAYOUT_PASS");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--partition-saved-tus")) {
        if (argc < 6) {
            completion_error("usage: --partition-saved-tus INPUT REQUESTS OUTPUT, followed by QEMU options");
        }
        input_path = argv[2];
        requests_path = argv[3];
        output_path = argv[4];
        for (int i = 5; i <= argc; i++) {
            argv[i - 4] = argv[i];
        }
        qemu_init(argc - 4, argv);
        if (!first_cpu) {
            completion_error("missing vCPU");
        }
        async_safe_run_on_cpu(first_cpu, partition_saved_tus, RUN_ON_CPU_NULL);
        qemu_main_loop();
        completion_error("vCPU batch partition callback did not finish");
    }
    pack_function_source =
        argc > 1 && !strcmp(argv[1], "--compile-function-code-pages");
    raw_source = argc > 1 && (pack_function_source ||
                              !strcmp(argv[1], "--compile-code-pages") ||
                              !strcmp(argv[1], "--compile-div-code-pages"));
    partition_owner = argc > 1 && !strcmp(argv[1], "--partition-saved-tu");
    page_source_only = argc > 1 &&
        (raw_source || !strcmp(argv[1], "--compile-function-pages") ||
         !strcmp(argv[1], "--compile-div-exception"));
    aot_offline_div_exception = argc > 1 &&
        (!strcmp(argv[1], "--compile-div-exception") ||
         !strcmp(argv[1], "--compile-div-code-pages"));
    function_owner = page_source_only ||
        (argc > 1 && !strcmp(argv[1], "--retranslate-function-tu"));
    retranslate_owner = partition_owner || function_owner ||
        (argc > 1 && !strcmp(argv[1], "--retranslate-saved-tu"));
    if (argc < 2 || (!retranslate_owner && strcmp(argv[1], "--complete-saved-tbs"))) {
        return 2;
    }
    int consumed = retranslate_owner && !partition_owner && !function_owner ? 4 : 5;
    if (argc < consumed + 2) {
        completion_error("usage: --complete-saved-tbs, --retranslate-function-tu or --compile-function-pages INPUT OWNER REQUESTS OUTPUT; --retranslate-saved-tu INPUT OWNER OUTPUT, followed by QEMU options");
    }
    input_path = argv[2];
    if (raw_source) {
        parse_code_page_mode(argv[3]);
    } else {
        owner_id = request_number(argv[3], 0);
    }
    requests_path = retranslate_owner && !partition_owner && !function_owner ? NULL : argv[4];
    output_path = argv[consumed];
    for (int i = consumed + 1; i <= argc; i++) {
        argv[i - consumed] = argv[i];
    }
    qemu_init(argc - consumed, argv);
    if (!first_cpu) {
        completion_error("missing vCPU");
    }
    async_safe_run_on_cpu(first_cpu, complete_blocks, RUN_ON_CPU_NULL);
    qemu_main_loop();
    completion_error("vCPU completion callback did not finish");
}

bool aot_offline_mode(int argc, char **argv)
{
    static const char *const modes[] = {
        "--validate-saved-aot",          "--partition-saved-tus",
        "--partition-saved-tu",          "--compile-function-pages",
        "--compile-div-exception",       "--compile-code-pages",
        "--compile-function-code-pages", "--compile-div-code-pages",
        "--compile-code-pages-batch",
        "--retranslate-function-tu",     "--retranslate-saved-tu",
        "--complete-saved-tbs",
    };
    if (argc < 2) {
        return false;
    }
    for (size_t i = 0; i < G_N_ELEMENTS(modes); i++) {
        if (!strcmp(argv[1], modes[i])) {
            return true;
        }
    }
    return false;
}
