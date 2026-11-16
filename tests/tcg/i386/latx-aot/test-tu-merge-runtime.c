/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Link against the system binary's objects: decoder and codegen are real. */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "sysemu/sysemu.h"
#include "sysemu/cpus.h"
#include "sysemu/runstate.h"
#include "exec/address-spaces.h"
#include "exec/cputlb.h"
#include "accel/tcg/internal.h"
#include "tcg/tcg-split-tb.h"
#include "sys-aot-merge.h"
#include "lsenv.h"
#include "latx-ras.h"
#include "latx-misc.h"
#include <ucontext.h>
#include "target/i386/latx/sbt/sys-aot-tu-merge.c"

static uint8_t guest_pages[12][TARGET_PAGE_SIZE];
static uint64_t guest_base = 0x400000;
static uint32_t entry_offsets[12];
static uint32_t entry_sizes[12];

static MergePlan new_plan(void)
{
    return (MergePlan) {
        .pages = g_hash_table_new_full(merge_page_hash, merge_page_equal,
                                       g_free, NULL),
        .tus = g_ptr_array_new_with_free_func(g_free),
        .groups = g_ptr_array_new_with_free_func(merge_group_free),
        .outputs = g_ptr_array_new_with_free_func(g_free),
        .spool = tmpfile(),
    };
}

static void free_plan(MergePlan *p)
{
    fclose(p->spool);
    g_free(p->copy_buffer);
    g_ptr_array_free(p->outputs, true);
    g_ptr_array_free(p->groups, true);
    g_ptr_array_free(p->tus, true);
    g_hash_table_destroy(p->pages);
}

static void copy_on_small_stack(void)
{
    const size_t size = 3 * 64 * 1024 + 17;
    uint8_t *bytes = g_malloc(size);
    uint8_t *actual = g_malloc(size);
    MergePlan plan = new_plan();
    FILE *out = tmpfile();

    assert(out);
    for (size_t i = 0; i < size; i++) {
        bytes[i] = (i * 31 + i / 251) & 255;
    }
    assert(fwrite(bytes, 1, 7, plan.spool) == 7);
    assert(fwrite(bytes, 1, size, plan.spool) == size);
    assert(!fflush(plan.spool));
    for (unsigned i = 0; i < 4; i++) {
        MergeOutput tu = {
            .original = (i & 1) ? NULL : bytes,
            .offset = 7,
            .size = size,
        };

        assert(merge_copy_output(&plan, out, &tu));
    }
    rewind(out);
    for (unsigned i = 0; i < 4; i++) {
        assert(fread(actual, 1, size, out) == size);
        assert(!memcmp(bytes, actual, size));
    }
    assert(fgetc(out) == EOF);
    fclose(out);
    free_plan(&plan);
    g_free(actual);
    g_free(bytes);
}

static void check_copy_stack_usage(void)
{
    /* HAMT fake-fork callbacks run on a four-host-page (64 KiB) stack. */
    const size_t size = 64 * 1024;
    size_t guard = MAX(size, qemu_real_host_page_size());
    void *mapping = mmap(NULL, size + 2 * guard, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ucontext_t caller, callee;

    assert(mapping != MAP_FAILED);
    assert(!mprotect(mapping + guard, size, PROT_READ | PROT_WRITE));
    assert(!getcontext(&callee));
    callee.uc_stack.ss_sp = mapping + guard;
    callee.uc_stack.ss_size = size;
    callee.uc_stack.ss_flags = 0;
    callee.uc_link = &caller;
    makecontext(&callee, copy_on_small_stack, 0);
    assert(!swapcontext(&caller, &callee));
    assert(!munmap(mapping, size + 2 * guard));
    fprintf(stderr, "PASS: original/spooled TU copy on guarded 64 KiB stack\n");
}

/* Two files can partition the same chain into alternating two-member TUs. */
static void check_group_scaling(unsigned count, bool duplicates)
{
    MergePlan plan = new_plan();
    MergePage *pages = g_new0(MergePage, count + 1);
    aot_tu_head *heads = g_new0(aot_tu_head, count);
    aot_tu_page *deps = g_new0(aot_tu_page, count * 2);
    aot_block *blocks = g_new0(aot_block, count * 2);

    for (unsigned i = 0; i <= count; i++) {
        pages[i].hash = i + 1;
    }
    for (unsigned i = 0; i < count; i++) {
        unsigned first = duplicates ? 0 :
            (i < count / 2 ? i * 2 : (i - count / 2) * 2 + 1);
        MergeTU *tu = g_new0(MergeTU, 1);
        MergeGroup *group = g_new0(MergeGroup, 1);

        group->tus = g_ptr_array_new();
        group->mappings = g_array_new(false, false, sizeof(MergeMapping));
        heads[i].first_pc = 0x400000 + (uint64_t)first * TARGET_PAGE_SIZE;
        heads[i].tb_count = heads[i].page_count = 2;
        heads[i].tu_id = duplicates ? 1 : i + 1;
        tu->head = &heads[i];
        tu->deps = &deps[i * 2];
        tu->blocks = &blocks[i * 2];
        tu->group = group;
        for (unsigned j = 0; j < 2; j++) {
            deps[i * 2 + j].pc_delta = j * TARGET_PAGE_SIZE;
            blocks[i * 2 + j].pc = heads[i].first_pc + j * TARGET_PAGE_SIZE;
            tu->pages[j] = &pages[first + j];
            MergeMapping mapping = { blocks[i * 2 + j].pc, tu->pages[j] };

            g_array_append_val(group->mappings, mapping);
        }
        g_ptr_array_add(group->tus, tu);
        g_ptr_array_add(plan.groups, group);
        g_ptr_array_add(plan.tus, tu);
    }
    gint64 start = g_get_monotonic_time();

    assert(merge_plan_groups(&plan));
    fprintf(stderr, "Grouping %u %s TUs: %.6f seconds\n", count,
            duplicates ? "duplicate" : "chained",
            (g_get_monotonic_time() - start) / 1000000.0);
    MergeGroup *merged = ((MergeTU *)g_ptr_array_index(plan.tus, 0))->group;
    unsigned expected = duplicates ? 2 : count + 1;

    assert(merged->tus->len == count);
    assert(merged->mappings->len == expected);
    assert(g_hash_table_size(merged->page_index) == expected);
    assert(g_hash_table_size(merged->member_index) == expected);
    for (unsigned i = 0; i < count; i++) {
        MergeTU *tu = g_ptr_array_index(plan.tus, i);

        assert(tu->group == merged);
        assert(tu->bias == 0);
    }
    free_plan(&plan);
    g_free(pages);
    g_free(heads);
    g_free(deps);
    g_free(blocks);
}

static void *read_output(FILE *file, uint64_t *size)
{
    assert(!fflush(file));
    *size = ftello(file);
    assert(*size > sizeof(aot_tail));
    void *data = g_malloc(*size);

    rewind(file);
    assert(fread(data, 1, *size, file) == *size);
    assert(validate_aot_layout(data, *size));
    return data;
}

/* Produce actual independent AOT inputs using the file-backed translator. */
static void *make_input_bounded(int first, int count, uint64_t bias,
                                int max_insns, uint64_t *size)
{
    MergePlan p = new_plan();
    MergeGroup *group = g_new0(MergeGroup, 1);
    MergeTU *tu = g_new0(MergeTU, 1);
    aot_tu_head head = { .tb_count = count };
    aot_block *blocks = g_new0(aot_block, count);
    FILE *out = tmpfile();
    int pages, tbs;

    assert(p.spool && out);
    group->tus = g_ptr_array_new();
    group->mappings = g_array_new(false, false, sizeof(MergeMapping));
    tu->head = &head;
    tu->blocks = blocks;
    tu->group = group;
    g_ptr_array_add(p.tus, tu);
    g_ptr_array_add(p.groups, group);
    g_ptr_array_add(group->tus, tu);
    for (int i = 0; i < count; i++) {
        int id = first + i;
        MergeMapping m = {
            guest_base + id * TARGET_PAGE_SIZE + bias,
            merge_page_intern(&p, guest_pages[id]),
        };

        g_array_append_val(group->mappings, m);
        blocks[i].pc = m.pc + entry_offsets[id];
        blocks[i].flags = HF_CS32_MASK | HF_SS32_MASK | HF_PE_MASK |
                          HF_CPL_MASK;
        blocks[i].cflags = 0;
        blocks[i].size = entry_sizes[id] ? entry_sizes[id] :
                        entry_offsets[id] ? 7 :
                        max_insns == 1 ? 5 : 10;
        blocks[i].icount = max_insns;
    }
    assert(merge_translate_group(&p, group));
    out_tb_file = out;
    assert(merge_write_plan(&p, out, AOT_USER, &pages, &tbs));
    assert(tbs == count);
    void *data = read_output(out, size);

    fclose(out);
    out_tb_file = NULL;
    free_plan(&p);
    g_free(blocks);
    return data;
}

static void *make_input(int first, int count, uint64_t bias, uint64_t *size)
{
    return make_input_bounded(first, count, bias, 2, size);
}

static void check_different_tb_lengths(void)
{
    uint64_t short_size, long_size, size;
    void *short_tu = make_input_bounded(0, 1, 0, 1, &short_size);
    void *long_tu = make_input(0, 1, 0, &long_size);
    FILE *out = tmpfile();
    int pages, tbs;
    MergePlan plan = new_plan();

    out_tb_file = out;
    assert(aot_merge_tu_files(short_tu, short_size, long_tu, long_size,
                             out, AOT_USER, &pages, &tbs));
    void *data = read_output(out, &size);

    assert(merge_parse_input(&plan, data, size));
    assert(plan.tus->len == 1);
    MergeTU *tu = g_ptr_array_index(plan.tus, 0);

    assert(tu->blocks[0].size == 10 && tu->blocks[0].icount == 2);
    free_plan(&plan);
    g_free(data);
    g_free(short_tu);
    g_free(long_tu);
    fclose(out);
    out_tb_file = NULL;
    fprintf(stderr, "PASS: same entry with different TB bounds "
            "is retranslated\n");
}

static void check_members(const void *data, uint64_t size,
                          int expected_tbs, int expected_tus)
{
    MergePlan p = new_plan();
    GHashTable *members = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                                g_free, NULL);
    int count = 0;

    assert(merge_parse_input(&p, data, size));
    assert(p.tus->len == expected_tus);
    for (guint i = 0; i < p.tus->len; i++) {
        MergeTU *tu = g_ptr_array_index(p.tus, i);

        for (uint32_t j = 0; j < tu->head->tb_count; j++) {
            uint64_t pc = tu->blocks[j].pc;

            assert(!g_hash_table_contains(members, &pc));
            g_hash_table_add(members, g_memdup2(&pc, sizeof(pc)));
            count++;
        }
    }
    assert(count == expected_tbs);
    g_hash_table_destroy(members);
    free_plan(&p);
}

static void *merge_inputs(void *a, uint64_t as, void *b, uint64_t bs,
                          uint64_t *size, int expected_tbs, int expected_tus)
{
    FILE *out = tmpfile();
    int pages = 0, tbs = 0;

    out_tb_file = out;
    assert(aot_merge_tu_files(a, as, b, bs, out, AOT_USER, &pages, &tbs));
    assert(tbs == expected_tbs);
    void *data = read_output(out, size);

    check_members(data, *size, expected_tbs, expected_tus);
    fclose(out);
    out_tb_file = NULL;
    return data;
}

static void check_observed_call_merge(void)
{
    uint64_t first_size, second_size, size;
    void *first = make_input(0, 1, 0, &first_size);
    void *second = make_input(0, 1, 0x100000, &second_size);
    MergePlan input = new_plan();

    assert(merge_parse_input(&input, second, second_size));
    MergeTU *marked = g_ptr_array_index(input.tus, 0);
    ((aot_block *)marked->blocks)[0].misc_flags |= MF_OBSERVED_CALL_ENTRY;
    free_plan(&input);
    /* The unmarked covering template comes first. It must not discard the
     * observation in an identical, relocated recording. Repeat with a later
     * unmarked duplicate to check that evidence only accumulates. */
    void *merged = merge_inputs(first, first_size, second, second_size,
                                &size, 1, 1);
    for (int pass = 0; pass < 2; pass++) {
        MergePlan output = new_plan();
        assert(merge_parse_input(&output, merged, size));
        MergeTU *tu = g_ptr_array_index(output.tus, 0);
        assert(tu->blocks[0].misc_flags & MF_OBSERVED_CALL_ENTRY);
        free_plan(&output);
        if (!pass) {
            uint64_t next_size;
            void *next = merge_inputs(first, first_size, merged, size,
                                       &next_size, 1, 1);
            g_free(merged);
            merged = next;
            size = next_size;
        }
    }
    g_free(merged);
    g_free(first);
    g_free(second);
    fprintf(stderr, "PASS: observed CALL survives relocated duplicate merges\n");
}

static void write_cache(const char *name, const void *data, uint64_t size)
{
    char *path = g_strdup_printf("%s%s", aot_file_path, name);
    FILE *file = fopen(path, "wb");

    assert(file && fwrite(data, 1, size, file) == size);
    assert(!fclose(file));
    g_free(path);
}

static void check_cache(const char *name, const void *data, uint64_t size)
{
    char *path = g_strdup_printf("%s%s", aot_file_path, name);
    gchar *actual;
    gsize actual_size;

    assert(g_file_get_contents(path, &actual, &actual_size, NULL));
    assert(actual_size == size && !memcmp(actual, data, size));
    g_free(actual);
    g_free(path);
}

/* Keep the layout valid while making a saved instruction undecodable. */
static void poison_code(void *data, uint64_t size, int id)
{
    aot_tail *tail = (void *)((uint8_t *)data + size - sizeof(*tail));
    page_info_array_t *index = (void *)((uint8_t *)tail - sizeof(*index) -
                                       tail->page_num * sizeof(page_info_t));
    uint64_t old_fp = 0, new_fp = 0;

    for (uint32_t i = 0; i < index->page_count; i++) {
        page_head *ph = (void *)((uint8_t *)data + index->pages[i].file_offset);
        uint8_t *bytes = (void *)(ph + 1);

        if (!memcmp(bytes, guest_pages[id], TARGET_PAGE_SIZE)) {
            old_fp = get_page_fingerprint(bytes);
            /* No opcode within the instruction limit. */
            memset(bytes, 0x66, 15);
            new_fp = get_page_fingerprint(bytes);
            ph->page_hash = get_page_hash((uint32_t *)bytes);
            index->pages[i].page_hash = ph->page_hash;
        }
    }
    assert(old_fp && new_fp && old_fp != new_fp);
    for (uint32_t i = 0; i < index->page_count; i++) {
        page_head *ph = (void *)((uint8_t *)data + index->pages[i].file_offset);
        uint8_t *record = (uint8_t *)(ph + 1) + TARGET_PAGE_SIZE;

        for (uint32_t j = 0; j < ph->tb_count; j++) {
            if (*(uint32_t *)record == AOT_TB_MAGIC) {
                aot_tu_head *head = (void *)record;
                aot_tu_page *deps = (void *)(head + 1);

                for (uint32_t k = 0; k < head->page_count; k++) {
                    if (deps[k].page_fingerprint == old_fp) {
                        deps[k].page_fingerprint = new_fp;
                    }
                }
                record += head->tu_sum_size;
            } else {
                record += sizeof(aot_tu_ref);
            }
        }
    }
    assert(validate_aot_layout(data, size));
}

static void check_page_end_decode(void)
{
    for (int variant = 0; variant < 3; variant++) {
        uint8_t bytes[2][TARGET_PAGE_SIZE] = { { 0 } };
        MergePlan plan = new_plan();
        MergeGroup *group = g_new0(MergeGroup, 1);
        MergeTU *tu = g_new0(MergeTU, 1);
        aot_tu_head head = { .tb_count = 1 };
        aot_block block = {
            .pc = guest_base + TARGET_PAGE_SIZE - 1,
            .flags = HF_CS32_MASK | HF_SS32_MASK | HF_PE_MASK | HF_CPL_MASK,
            .size = 1, .icount = 1,
        };

        /* A short saved bound makes decode discover the second page. */
        bytes[0][TARGET_PAGE_SIZE - 1] = variant ? 0xb8 : 0x90;
        group->tus = g_ptr_array_new();
        group->mappings = g_array_new(false, false, sizeof(MergeMapping));
        tu->head = &head;
        tu->blocks = &block;
        tu->group = group;
        g_ptr_array_add(plan.tus, tu);
        g_ptr_array_add(plan.groups, group);
        g_ptr_array_add(group->tus, tu);
        for (int i = 0; i < (variant == 2 ? 2 : 1); i++) {
            MergeMapping m = { guest_base + i * TARGET_PAGE_SIZE,
                                merge_page_intern(&plan, bytes[i]) };

            g_array_append_val(group->mappings, m);
        }
        assert(merge_translate_group(&plan, group) == (variant != 1));
        if (variant != 1) {
            MergeOutput *out = g_ptr_array_index(plan.outputs, 0);

            assert(out->page_count == (variant == 2 ? 2 : 1));
        }
        free_plan(&plan);
    }
    fprintf(stderr, "PASS: page-end instruction, absent continuation page, "
            "real cross-page decode\n");
}

static void check_repeated_pages(void)
{
    uint8_t saved[TARGET_PAGE_SIZE];
    uint64_t as, bs, ms;

    memcpy(saved, guest_pages[1], sizeof(saved));
    memcpy(guest_pages[1], guest_pages[0], sizeof(saved));
    void *a = make_input(0, 2, 0, &as);
    void *merged = merge_inputs(a, as, a, as, &ms, 2, 1);
    void *b = make_input(0, 3, 0, &bs);
    MergePlan plan = new_plan();

    assert(merge_parse_input(&plan, a, as));
    assert(merge_parse_input(&plan, b, bs));
    assert(merge_parse_input(&plan, a, as));
    assert(merge_plan_groups(&plan));
    MergeTU *first = g_ptr_array_index(plan.tus, 0);
    MergeTU *different = g_ptr_array_index(plan.tus, 1);
    MergeTU *duplicate = g_ptr_array_index(plan.tus, 2);

    assert(first->group == duplicate->group);
    assert(first->group != different->group);
    free_plan(&plan);
    g_free(b);

    g_free(merged);
    g_free(a);
    memcpy(guest_pages[1], saved, sizeof(saved));
    fprintf(stderr, "PASS: repeated page bytes preserve relative mappings "
            "and duplicate TU idempotence\n");
}

static void check_failures(void)
{
    uint64_t as, bs;
    void *a = make_input(0, 3, 0, &as);
    void *b = make_input(2, 3, 0, &bs);
    FILE *full = fopen("/dev/full", "wb");
    int pages, tbs;

    assert(full && !setvbuf(full, NULL, _IONBF, 0));
    out_tb_file = full;
    assert(!aot_merge_tu_files(b, bs, a, as, full, AOT_USER, &pages, &tbs));
    fclose(full);
    out_tb_file = NULL;
    poison_code(b, bs, 4);
    write_cache("sys-aot-user", a, as);
    write_cache("sys-aot-user-add1", b, bs);
    assert(aot_merge_all("sys-aot-user", AOT_USER) < 0);
    check_cache("sys-aot-user", a, as);
    check_cache("sys-aot-user-add1", b, bs);
    g_free(a);
    g_free(b);

    /* C overlaps, but different B code makes the two dependencies conflict. */
    a = make_input(0, 3, 0, &as);
    guest_pages[1][1] ^= 0x10;
    b = make_input(1, 3, 0, &bs);
    guest_pages[1][1] ^= 0x10;
    FILE *out = tmpfile();

    out_tb_file = out;
    assert(aot_merge_tu_files(b, bs, a, as, out, AOT_USER, &pages, &tbs));
    assert(tbs == 6 && pages == 5);
    uint64_t size;
    void *data = read_output(out, &size);
    MergePlan plan = new_plan();

    assert(merge_parse_input(&plan, data, size) && plan.tus->len == 2);
    free_plan(&plan);
    g_free(data);
    fclose(out);
    out_tb_file = NULL;
    g_free(a);
    g_free(b);
    fprintf(stderr, "PASS: output failure, decode failure preserves inputs, "
            "conflicting code versions retained\n");
}

static QEMUTimer *scheduler_timer;

static void poll_scheduler(void *opaque)
{
    static unsigned polls;
    AOTPublishedFile *file;
    bool complete = false;
    char *add_name = g_strdup_printf("%ssys-aot-user-add1", aot_file_path);

    rcu_read_lock();
    file = qatomic_rcu_read(&user_aot_file);
    if (file) {
        aot_tail *tail = (void *)((uint8_t *)file->tb_ptr + file->size -
                                  sizeof(*tail));

        complete = tail->tb_num == 5 && access(add_name, F_OK) < 0 &&
                   !aot_tu_merge_running();
    }
    rcu_read_unlock();
    g_free(add_name);
    if (complete) {
        aot_merger_stop();
        fprintf(stderr, "PASS: background request, safe vCPU callback, "
                "fork translation, child reap and publication\n");
        fflush(NULL);
        _exit(0);
    }
    assert(++polls < 350);
    /* Refresh requests here would hide lost child completion notifications. */
    if (!g_getenv("AOT_TEST_PARTIAL_MERGE")) {
        aot_merger_request(AOT_USER);
    }
    timer_mod(scheduler_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100);
}

static void wait_shutdown_save(void)
{
    int status;
    pid_t pid;

    do {
        pid = waitpid(-1, &status, 0);
    } while (pid < 0 && errno == EINTR);
    assert(pid > 0 && WIFEXITED(status));
    assert((WEXITSTATUS(status) & ~3) == 64);
}

static void start_scheduler(CPUState *cpu)
{
    uint64_t as, bs;
    void *a = make_input(0, 3, 0, &as);
    void *b = make_input(2, 3, 0x100000, &bs);

    write_cache("sys-aot-user", a, as);
    write_cache("sys-aot-user-add1", b, bs);
    g_free(a);
    g_free(b);
    in_aot_translate = 0;
    aot_tu_merge_cpu = NULL;
    if (g_getenv("AOT_TEST_PARTIAL_MERGE")) {
        write_cache("sys-aot-user-add2", "bad", 3);
        user_aot_file = load_aot_file("sys-aot-user", AOT_USER);
        assert(user_aot_file);
    }
    if (g_getenv("AOT_TEST_MAIN_SHUTDOWN")) {
        /* Exit before a save/merge callback captures any vCPU context. */
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
        return;
    }
    aot_merger_start();
    if (g_getenv("AOT_TEST_SHUTDOWN")) {
        aot_request_tu_merge(true);
        aot_save_on_shutdown();
        assert(!aot_tu_merge_running());
        wait_shutdown_save();
        char *path = g_strdup_printf("%ssys-aot-user", aot_file_path);
        gchar *data;
        gsize size;

        assert(g_file_get_contents(path, &data, &size, NULL));
        check_members(data, size, 5, 1);
        g_free(data);
        g_free(path);
        fprintf(stderr, "PASS: queued request at shutdown, worker stop "
                "and asynchronous drain without new TBs\n");
        fflush(NULL);
        _exit(0);
    }
    scheduler_timer = timer_new_ms(QEMU_CLOCK_REALTIME, poll_scheduler, NULL);
    timer_mod(scheduler_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 100);
}

static void check_load_and_execute(CPUState *cpu, int mode)
{
    bool arithmetic = mode == 1;
    bool branch = mode == 2;
    bool call = mode == 3;
    CPUX86State *env = cpu->env_ptr;
    uint32_t flags = HF_CS32_MASK | HF_SS32_MASK | HF_PE_MASK | HF_CPL_MASK;
    uint64_t as, bs;

    in_aot_translate = 1;
    aot_tu_merge_cpu = cpu;
    void *a = make_input(0, 3, 0, &as);
    void *b = make_input(2, 3, 0x100000, &bs);
    char *add_name = g_strdup_printf("%ssys-aot-user-add1", aot_file_path);

    write_cache("sys-aot-user", a, as);
    write_cache("sys-aot-user-add1", b, bs);
    aot_tu_merge_cpu = NULL;
    assert(aot_merge_all("sys-aot-user", AOT_USER) == -3);
    assert(access(add_name, F_OK) == 0);
    aot_tu_merge_cpu = cpu;
    assert(aot_merge_all("sys-aot-user", AOT_USER) == 1);
    assert(access(add_name, F_OK) < 0 && errno == ENOENT);
    g_free(add_name);
    g_free(a);
    g_free(b);

    for (int i = 0; i < 5; i++) {
        cpu_physical_memory_write(guest_base + i * TARGET_PAGE_SIZE,
                                   guest_pages[i], TARGET_PAGE_SIZE);
    }
    env->hflags = flags;
    env->cr[0] = CR0_PE_MASK | CR0_ET_MASK;
    env->segs[R_CS].base = 0;
    env->segs[R_SS].base = 0;
    env->eip = guest_base;
    tlb_flush(cpu);
    aot_tu_merge_cpu = NULL;
    in_aot_translate = 0;
    option_aot_load = 1;
    user_aot_file = load_aot_file("sys-aot-user", AOT_USER);
    assert(user_aot_file);
    rcu_read_lock();
    /* Load through a non-anchor member, then execute every member entry. */
    TranslationBlock *loaded = load_aot(cpu, guest_base + 3 * TARGET_PAGE_SIZE,
                                        0, flags, 0, lsenv);

    assert(loaded && (loaded->bool_flags & IS_AOT_TB));
    for (int i = 0; i < 5; i++) {
        uint64_t pc = guest_base + i * TARGET_PAGE_SIZE + entry_offsets[i];
        TranslationBlock *tb = tb_htable_lookup(cpu, pc, 0, flags, 0);
        uintptr_t ret;
        TranslationBlock *last;

        /* Exercise the callee's RET through CALL, with a real RAS entry. */
        if (call && i == 4) {
            continue;
        }
        if (!tb && call) {
            tb = load_aot(cpu, pc, 0, flags, 0, lsenv);
        }
        assert(tb && (tb->bool_flags & IS_AOT_TB));
        env->eip = pc;
        env->regs[R_EAX] = arithmetic ? UINT32_MAX : 0;
        env->regs[R_ECX] = 3;
        env->regs[R_ESP] = 0xffff00;
        if (call) {
            uint32_t return_pc = guest_base + 5 * TARGET_PAGE_SIZE;

            cpu_physical_memory_write(env->regs[R_ESP], &return_pc, 4);
            latxs_flush_soft_ras(cpu);
            cpu->s_ras->top = 0;
        }
        env->cc_op = CC_OP_EFLAGS;
        env->cc_src = 0;
        for (unsigned dispatch = 0; ; dispatch++) {
            assert(dispatch < 16);
            cpu->curr_exec_tb = tb;
            cpu->exit_request = 0;
            cpu_neg(cpu)->icount_decr.u16.high = 0;
            qemu_thread_jit_execute();
            if (call) {
                /* Match cpu_tb_exec's software-RAS register handoff. */
                Soft_RAS *ras = cpu->s_ras;
                uintptr_t top = (uintptr_t)&ras->stack[0];

#ifndef CONFIG_RAS_SLOW
                top += ras->top * sizeof(RAS_Entry);
#endif
                __asm__ volatile ("movgr2scr $scr0, %0" :: "r"(top));
            }
            latxs_before_exec_tb(cpu, tb);
            ret = tcg_qemu_tb_exec2[curr_mtcc(env)](tb->tc.ptr, env);
            latxs_after_exec_tb(cpu, tb);
#ifndef CONFIG_RAS_SLOW
            if (call) {
                uintptr_t top;
                Soft_RAS *ras = cpu->s_ras;

                __asm__ volatile ("movscr2gr %0, $scr0" : "=r"(top));
                ras->top = (top - (uintptr_t)&ras->stack[0]) /
                           sizeof(RAS_Entry);
            }
#endif
            assert((ret & TB_EXIT_MASK) <= TB_EXIT_IDX1 ||
                   (call && (ret & TB_EXIT_MASK) == TB_EXIT_REQUESTED));
            last = (void *)(ret & ~TB_EXIT_MASK);
            if (option_split_tb && split_tb_base_ok()) {
                last = (void *)((uintptr_t)last | SPLIT_TB_BASE);
            } else if (!option_split_tb) {
                last = tcg_splitwx_to_rw(last);
            }
            /* cpu_tb_exec normally consumes the lazy-PC exit record. */
            if (option_lazypc && last &&
                !check_misc_flags(last, MF_END_SYSCALL) &&
                (ret & TB_EXIT_MASK) <= TB_EXIT_IDX1) {
                env->eip = last->next_pc[ret & TB_EXIT_MASK];
            }
            if (env->eip == guest_base + 5 * TARGET_PAGE_SIZE) {
                break;
            }
            /* Backedges and unoptimized exits can return to the dispatcher. */
            tb = tb_htable_lookup(cpu, env->eip, 0, flags, 0);
            if (!tb && call) {
                tb = load_aot(cpu, env->eip, 0, flags, 0, lsenv);
            }
            if (!tb) {
                fprintf(stderr, "Missing AOT entry: mode=%d start=%d "
                        "pc=%" PRIx64 " exit=%lu last=%" PRIx64 "\n",
                        mode, i, env->eip, ret & TB_EXIT_MASK,
                        last ? last->pc : 0);
            }
            assert(tb && (tb->bool_flags & IS_AOT_TB));
        }
        uint32_t expected = 5;

        if (arithmetic) {
            expected = UINT32_MAX;
            unsigned carry = 0;

            for (int j = i; j < 5; j++) {
                uint64_t sum = (uint64_t)expected + j + 1 +
                               ((j & 1) ? carry : 0);

                expected = sum;
                carry = sum >> 32;
            }
        } else if (branch) {
            expected = i < 2 ? 15 - i : i == 2 ? 12 : i == 3 ? 9 : 5;
            assert(env->regs[R_ECX] == (i < 2 ? 0 : 3));
        } else if (call) {
            expected = i == 0 ? 14 : i == 1 ? 9 :
                       i == 2 ? 7 : i == 3 ? 4 : 5;
            assert(env->regs[R_ESP] == 0xffff00 + (i == 4 ? 4 : 0));
        }
        assert(env->regs[R_EAX] == expected);
        assert(env->eip == guest_base + 5 * TARGET_PAGE_SIZE);
    }
    cpu->curr_exec_tb = NULL;
    fprintf(stderr, "PASS: member-page load and "
            "AOT control-flow execution (%s)\n",
            call ? "CALL/RET with software RAS enabled" :
            branch ? "JCC taken/fallthrough and cross-page backedge" :
            arithmetic ? "ADD/ADC carry across TBs" : "MOV/JMP");
    rcu_read_unlock();
}

typedef struct JITEdgeResult {
    unsigned tus;
    unsigned tbs;
    unsigned cross_page_tus;
} JITEdgeResult;

static AOTCrossPageTUResult check_jit_edge_tu(TranslationBlock **tbs,
                                              int count, void *opaque)
{
    JITEdgeResult *result = opaque;

    assert(count == 1 || count == 2);
    assert(tbs[0]->tc.ptr && tbs[0]->s_data->tu_size);
    result->tus++;
    result->tbs += count;
    if (count == 2) {
        assert((tbs[0]->pc & TARGET_PAGE_MASK) !=
               (tbs[1]->pc & TARGET_PAGE_MASK));
        result->cross_page_tus++;
    }
    return AOT_TU_SAVE_OK;
}

static void check_jit_runtime_edges(CPUState *cpu)
{
    option_aot_tu_pretranslate = 0;
    CPUX86State *env = cpu->env_ptr;
    uint32_t flags = HF_CS32_MASK | HF_SS32_MASK | HF_PE_MASK | HF_CPL_MASK;
    uint8_t code[TARGET_PAGE_SIZE];

    env->hflags = flags;
    env->cr[0] = CR0_PE_MASK | CR0_ET_MASK;
    env->segs[R_CS].base = env->segs[R_SS].base = 0;
    rcu_read_lock();
    /* JCC target, JCC fallthrough and JMP, with and without a usable link. */
    for (int variant = 0; variant < 6; variant++) {
        int kind = variant % 3;
        bool linked = variant < 3;
        target_ulong page = 0x1000000 + variant * 0x10000;
        target_ulong pc = page + (kind == 1 ? TARGET_PAGE_SIZE - 6 : 0);
        int edge = kind == 1 ? TU_TB_INDEX_NEXT : TU_TB_INDEX_TARGET;
        TranslationBlock *templates[2];
        JITEdgeResult result = { 0 };
        int length = kind == 2 ? 5 : 6;
        int32_t displacement = kind == 1 ? TARGET_PAGE_SIZE :
                               TARGET_PAGE_SIZE - length;
        uint8_t *branch = code + (pc - page);

        in_aot_translate = 0;
        option_sys_aot = option_aot_load = 0;
        memset(code, 0x90, sizeof(code));
        if (kind == 2) {
            branch[0] = 0xe9; /* JMP page B. */
        } else {
            branch[0] = 0x0f;
            branch[1] = 0x85; /* JNZ: target or fallthrough reaches page B. */
        }
        memcpy(branch + length - 4, &displacement, 4);
        cpu_physical_memory_write(page, code, sizeof(code));
        code[0] = 0xc3;
        cpu_physical_memory_write(page + TARGET_PAGE_SIZE, code, sizeof(code));
        env->eip = pc;
        tlb_flush(cpu);
        templates[0] = tb_gen_code(cpu, pc, 0, flags, 0);
        templates[1] = tb_gen_code(cpu, page + TARGET_PAGE_SIZE, 0, flags, 0);
        assert(templates[0] && templates[1]);
        assert(templates[0]->s_data == templates[1]->s_data);
        /* Supply only the link, never TU-only metadata on the JIT templates. */
        templates[0]->jmp_dest[edge] = linked ? (uintptr_t)templates[1] : 1;
        uintptr_t raw_target = templates[0]->s_data->x86_target_pc;
        uintptr_t raw_next = templates[0]->s_data->x86_next_pc;

        option_sys_aot = 1;
        in_aot_translate = 1;
        assert(retranslate_by_tu_cross_page(cpu, templates, 2,
                                             check_jit_edge_tu, &result));
        assert(result.tbs == 2);
        assert(result.tus == (linked ? 1 : 2));
        assert(result.cross_page_tus == (linked ? 1 : 0));
        assert(templates[0]->s_data->x86_target_pc == raw_target);
        assert(templates[0]->s_data->x86_next_pc == raw_next);
        templates[0]->jmp_dest[edge] = 0;
    }
    rcu_read_unlock();
    fprintf(stderr, "PASS: real JIT TBs retranslate into multi-page AOT TUs "
            "(JCC target/fallthrough, JMP, detached-link rejection)\n");
}

static void run_test(CPUState *cpu, run_on_cpu_data unused)
{
    uint64_t as, bs, cs, ms, again_size;
    void *a, *b, *c, *merged, *again;

    offline_cpu = cpu;
    aot_tu_merge_cpu = cpu;
    in_aot_translate = 1;
    option_sys_aot = option_aot_save = 1;
    option_tu_mode = LATX_TU_MODE_AOT;
    option_aot_cross_page_tu = 1;
    aot_file_path = g_strdup_printf("%s/", g_getenv("AOT_TEST_DIR"));
    aot_reloc_init();
    if (g_getenv("AOT_TEST_JIT_EDGES")) {
        check_jit_runtime_edges(cpu);
        fflush(NULL);
        _exit(0);
    }
    for (int i = 0; i < 12; i++) {
        uint8_t *p = guest_pages[i];
        uint32_t imm = i + 1;
        int32_t rel = TARGET_PAGE_SIZE - 10;

        memset(p, 0x90, TARGET_PAGE_SIZE);
        p[0] = 0xb8; /* mov eax, unique immediate */
        memcpy(p + 1, &imm, sizeof(imm));
        p[5] = 0xe9; /* jmp next page */
        memcpy(p + 6, &rel, sizeof(rel));
    }
    if (g_getenv("AOT_TEST_SCHEDULER")) {
        start_scheduler(cpu);
        return;
    }
    check_page_end_decode();
    check_different_tb_lengths();
    check_observed_call_merge();
    check_repeated_pages();
    check_failures();
    a = make_input(0, 3, 0, &as);
    b = make_input(2, 3, 0x100000, &bs);
    merged = merge_inputs(b, bs, a, as, &ms, 5, 1);
    again = merge_inputs(b, bs, merged, ms, &again_size, 5, 1);
    assert(again_size == ms);
    assert(!memcmp(again, merged, ms));
    g_free(again);
    c = make_input(4, 2, 0, &cs);
    again = merge_inputs(c, cs, merged, ms, &again_size, 6, 1);
    g_free(again);
    g_free(merged);
    g_free(c);
    g_free(b);
    g_free(a);

    a = make_input(0, 8, 0, &as);
    b = make_input(7, 4, 0, &bs);
    merged = merge_inputs(b, bs, a, as, &ms, 11, 2);
    g_free(merged);
    g_free(b);
    g_free(a);
    option_soft_ras = 0;
    check_load_and_execute(cpu, 0);
    guest_base += 0x200000;
    for (int i = 0; i < 5; i++) {
        guest_pages[i][0] = (i & 1) ? 0x15 : 0x05; /* ADC/ADD eax, imm32 */
    }
    check_load_and_execute(cpu, 1);
    guest_base += 0x200000;
    for (int i = 0; i < 5; i++) {
        guest_pages[i][0] = 0x05; /* ADD eax, imm32 */
    }
    entry_offsets[1] = TARGET_PAGE_SIZE - 7;
    int32_t rel = 2 * TARGET_PAGE_SIZE - 17;

    memcpy(guest_pages[0] + 6, &rel, sizeof(rel));
    uint8_t *branch = guest_pages[1] + entry_offsets[1];

    branch[0] = 0x49; /* DEC ecx */
    branch[1] = 0x0f;
    branch[2] = 0x85; /* JNZ page 0; fallthrough reaches page 2 */
    rel = -2 * TARGET_PAGE_SIZE;
    memcpy(branch + 3, &rel, sizeof(rel));
    check_load_and_execute(cpu, 2);
    guest_base += 0x200000;
    memset(entry_offsets, 0, sizeof(entry_offsets));
    for (int i = 0; i < 5; i++) {
        uint32_t imm = i + 1;
        int32_t next = TARGET_PAGE_SIZE - 10;

        memset(guest_pages[i], 0x90, TARGET_PAGE_SIZE);
        guest_pages[i][0] = 0x05;
        memcpy(guest_pages[i] + 1, &imm, 4);
        guest_pages[i][5] = 0xe9;
        memcpy(guest_pages[i] + 6, &next, 4);
    }
    entry_offsets[0] = TARGET_PAGE_SIZE - 5;
    entry_sizes[0] = 5;
    guest_pages[0][entry_offsets[0]] = 0xe8; /* CALL page 4 */
    rel = 3 * TARGET_PAGE_SIZE;
    memcpy(guest_pages[0] + entry_offsets[0] + 1, &rel, 4);
    rel = 2 * TARGET_PAGE_SIZE - 10;
    memcpy(guest_pages[3] + 6, &rel, 4); /* Skip the callee at page 4. */
    guest_pages[4][5] = 0xc3; /* RET */
    entry_sizes[4] = 6;
    option_soft_ras = 1;
    assert(cpu->s_ras);
    check_load_and_execute(cpu, 3);
    fprintf(stderr, "PASS: real file decode/codegen, rebased overlap, "
            "idempotence, chained overlap, page-limit split, load/execute\n");
    fflush(NULL);
    _exit(0);
}

int main(int argc, char **argv)
{
    if (g_getenv("AOT_TEST_GROUPING")) {
        check_copy_stack_usage();
        check_group_scaling(64000, false);
        check_group_scaling(64000, true);
        option_sys_aot = 0;
        return 0;
    }
    qemu_init(argc, argv);
    assert(first_cpu);
    async_safe_run_on_cpu(first_cpu, run_test, RUN_ON_CPU_NULL);
    qemu_main_loop();
    if (g_getenv("AOT_TEST_MAIN_SHUTDOWN")) {
        /* Match the real shutdown thread, not the vCPU's populated TLS. */
        aot_save_on_shutdown();
        assert(!aot_tu_merge_running());
        wait_shutdown_save();
        char *path = g_strdup_printf("%ssys-aot-user", aot_file_path);
        gchar *data;
        gsize size;

        assert(g_file_get_contents(path, &data, &size, NULL));
        check_members(data, size, 5, 1);
        g_free(data);
        g_free(path);
        fprintf(stderr, "PASS: main-thread shutdown drains pending "
                "cross-page merge\n");
        fflush(NULL);
        _exit(0);
    }
    return 1;
}
