/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

struct tcg_region_tree {
    QemuMutex lock;
    QTree *tree;
    /* padding to avoid false sharing is computed at run-time */
};

/*
 * This is an array of struct tcg_region_tree's, with padding.
 * We use void * to simplify the computation of region_trees[i]; each
 * struct is found every tree_size bytes.
 */
static void *region_trees;
static size_t tree_size;

/* compare a pointer @ptr and a tb_tc @s */
static int ptr_cmp_tb_tc(const void *ptr, const struct tb_tc *s)
{
    if (ptr >= s->ptr + s->size) {
        return 1;
    } else if (ptr < s->ptr) {
        return -1;
    }
    return 0;
}

static gint tb_tc_cmp(gconstpointer ap, gconstpointer bp, gpointer userdata)
{
    const struct tb_tc *a = ap;
    const struct tb_tc *b = bp;

    /*
     * When both sizes are set, we know this isn't a lookup.
     * This is the most likely case: every TB must be inserted; lookups
     * are a lot less frequent.
     */
    if (likely(a->size && b->size)) {
        if (a->ptr > b->ptr) {
            return 1;
        } else if (a->ptr < b->ptr) {
            return -1;
        }
        /* a->ptr == b->ptr should happen only on deletions */
        g_assert(a->size == b->size);
        return 0;
    }
    /*
     * All lookups have either .size field set to 0.
     * From the glib sources we see that @ap is always the lookup key. However
     * the docs provide no guarantee, so we just mark this case as likely.
     */
    if (likely(a->size == 0)) {
        return ptr_cmp_tb_tc(a->ptr, b);
    }
    return ptr_cmp_tb_tc(b->ptr, a);
}

static void tcg_region_trees_init(void)
{
    size_t i;

    tree_size = ROUND_UP(sizeof(struct tcg_region_tree), qemu_dcache_linesize);
    region_trees = qemu_memalign(qemu_dcache_linesize, region.n * tree_size);
    for (i = 0; i < region.n; i++) {
        struct tcg_region_tree *rt = region_trees + i * tree_size;

        qemu_mutex_init(&rt->lock);
        rt->tree = q_tree_new_full(tb_tc_cmp, NULL, NULL, tb_destroy);
    }
}

static struct tcg_region_tree *tc_ptr_to_region_tree(const void *p)
{
    size_t region_idx;

    /*
     * Like tcg_splitwx_to_rw, with no assert.  The pc may come from
     * a signal handler over which the caller has no control.
     */
    if (!in_code_gen_buffer(p)) {
        p -= tcg_splitwx_diff;
        if (!in_code_gen_buffer(p)) {
            return NULL;
        }
    }

    if (p < region.start_aligned) {
        region_idx = 0;
    } else {
        ptrdiff_t offset = p - region.start_aligned;

        if (offset > region.stride * (region.n - 1)) {
            region_idx = region.n - 1;
        } else {
            region_idx = offset / region.stride;
        }
    }
    return region_trees + region_idx * tree_size;
}

void tcg_tb_insert(TranslationBlock *tb)
{
    struct tcg_region_tree *rt = tc_ptr_to_region_tree(tb->tc.ptr);

    g_assert(rt != NULL);
    qemu_mutex_lock_1(&rt->lock);
    q_tree_insert(rt->tree, &tb->tc, tb);
    qemu_mutex_unlock_1(&rt->lock);
}

void tcg_tb_remove(TranslationBlock *tb)
{
    struct tcg_region_tree *rt = tc_ptr_to_region_tree(tb->tc.ptr);

    g_assert(rt != NULL);
    qemu_mutex_lock_1(&rt->lock);
    q_tree_remove(rt->tree, &tb->tc);
    qemu_mutex_unlock_1(&rt->lock);
}

/*
 * Find the TB 'tb' such that
 * tb->tc.ptr <= tc_ptr < tb->tc.ptr + tb->tc.size
 * Return NULL if not found.
 */
TranslationBlock *tcg_tb_lookup(uintptr_t tc_ptr)
{
    struct tcg_region_tree *rt = tc_ptr_to_region_tree((void *)tc_ptr);
    TranslationBlock *tb;
    struct tb_tc s = { .ptr = (void *)tc_ptr };

    if (rt == NULL) {
        return NULL;
    }

    qemu_mutex_lock_1(&rt->lock);
    tb = q_tree_lookup(rt->tree, &s);
    qemu_mutex_unlock_1(&rt->lock);
    return tb;
}

static void tcg_region_tree_lock_all(void)
{
    size_t i;

    for (i = 0; i < region.n; i++) {
        struct tcg_region_tree *rt = region_trees + i * tree_size;

        qemu_mutex_lock_1(&rt->lock);
    }
}

static void tcg_region_tree_unlock_all(void)
{
    size_t i;

    for (i = 0; i < region.n; i++) {
        struct tcg_region_tree *rt = region_trees + i * tree_size;

        qemu_mutex_unlock_1(&rt->lock);
    }
}

void tcg_tb_foreach(GTraverseFunc func, gpointer user_data)
{
    size_t i;

    tcg_region_tree_lock_all();
    for (i = 0; i < region.n; i++) {
        struct tcg_region_tree *rt = region_trees + i * tree_size;

        q_tree_foreach(rt->tree, func, user_data);
    }
    tcg_region_tree_unlock_all();
}

size_t tcg_nb_tbs(void)
{
    size_t nb_tbs = 0;
    size_t i;

    tcg_region_tree_lock_all();
    for (i = 0; i < region.n; i++) {
        struct tcg_region_tree *rt = region_trees + i * tree_size;

        nb_tbs += q_tree_nnodes(rt->tree);
    }
    tcg_region_tree_unlock_all();
    return nb_tbs;
}

static void tcg_region_tree_reset_all(void)
{
    size_t i;

    tcg_region_tree_lock_all();
    for (i = 0; i < region.n; i++) {
        struct tcg_region_tree *rt = region_trees + i * tree_size;

        /* Increment the refcount first so that destroy acts as a reset */
        q_tree_ref(rt->tree);
        q_tree_destroy(rt->tree);
    }
    tcg_region_tree_unlock_all();
}

static void tcg_region_tree_reset_mtcc(uint32_t mtcc_id)
{
    size_t i, r;

    tcg_region_tree_lock_all();
    for (i = 0; i < region.mtcc_n; i++) {
        r = region.mtcc_n * mtcc_id + i;
        struct tcg_region_tree *rt = region_trees + r * tree_size;

        /* Increment the refcount first so that destroy acts as a reset */
        q_tree_ref(rt->tree);
        q_tree_destroy(rt->tree);
    }
    tcg_region_tree_unlock_all();
}
