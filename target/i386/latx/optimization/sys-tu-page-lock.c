/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "ir1.h"
#include "ir2.h"
#include "exec/exec-all.h"
#include "exec/cputlb.h"
#include "accel/tcg/internal.h"
#include "latx-options.h"
#include "latx-tu.h"
#include "latx-tu-page-lock.h"

#ifdef CONFIG_LATX_TU

/* Return the target-page-aligned address used by every TU page set. */
static inline uint64_t tu_page(uint64_t addr)
{
    return addr & TARGET_PAGE_MASK;
}

/*
 * Report whether this tu_gen_code() invocation may explore another page.
 * Lock-order retry exhaustion disables cross-page expansion for this run.
 */
bool tu_cross_page_enabled(void)
{
    return cp_tu() && !tu_data->disable_cross_page_for_run;
}

/* Return the current explore-page index for @vp, or -1 if it is unknown. */
int tu_pages_find(uint64_t vp)
{
    uint64_t page = tu_page(vp);

    for (int i = 0; i < tu_data->pages_num; i++) {
        if (tu_data->pages[i].vp == page) {
            return i;
        }
    }
    return -1;
}

/*
 * Record a virtual-to-physical page mapping in the current explore set.
 * A -1 physical address reserves the virtual-page slot without a mapping.
 */
static bool tu_pages_record(uint64_t vp, uint64_t pp)
{
    int i = tu_pages_find(vp);
    uint64_t page = tu_page(pp);

    if (i < 0) {
        if (tu_data->pages_num >= cp_tu_page_num()) {
            return false;
        }
        i = tu_data->pages_num++;
        tu_data->pages[i].vp = tu_page(vp);
        tu_data->pages[i].pp = -1;
        tu_data->pages[i].tb_num = 0;
    }

    if (pp != -1) {
        if (tu_data->pages[i].pp == -1) {
            tu_data->pages[i].pp = page;
        } else {
            assert(tu_data->pages[i].pp == page);
        }
    }
    return true;
}

/*
 * Reserve @vp for a possible TB and enforce both page and per-page TB limits.
 * No PageDesc lock is acquired by this bookkeeping operation.
 */
bool tu_pages_append(uint64_t vp)
{
    int i = tu_pages_find(vp);

    if (i >= 0) {
        return tu_data->pages[i].tb_num < MAX_TB_IN_TU;
    }
    if (tu_data->pages_num >= cp_tu_page_num()) {
        return false;
    }

    i = tu_data->pages_num++;
    tu_data->pages[i].vp = tu_page(vp);
    tu_data->pages[i].pp = -1;
    tu_data->pages[i].tb_num = 0;
    return true;
}

/* Return the held-lock index for @pp, or -1 if this TU does not hold it. */
static int tu_locked_pages_find(uint64_t pp)
{
    uint64_t page = tu_page(pp);

    for (int i = 0; i < tu_data->locked_pages_num; i++) {
        if (tu_data->locked_pages[i] == page) {
            return i;
        }
    }
    return -1;
}

/* Return the greatest physical page in the monotonically ordered lock set. */
static uint64_t tu_locked_pages_max(void)
{
    uint64_t max_pp = 0;

    for (int i = 0; i < tu_data->locked_pages_num; i++) {
        max_pp = MAX(max_pp, tu_data->locked_pages[i]);
    }
    return max_pp;
}

/* Save or update a virtual page that must survive a full explore restart. */
static bool tu_retry_pages_record(uint64_t vp, uint64_t pp)
{
    uint64_t virt_page = tu_page(vp);
    uint64_t phys_page = tu_page(pp);

    for (int i = 0; i < tu_data->retry_pages_num; i++) {
        if (tu_data->retry_pages[i].vp == virt_page) {
            tu_data->retry_pages[i].pp = phys_page;
            return true;
        }
    }
    if (tu_data->retry_pages_num >= cp_tu_page_num()) {
        return false;
    }

    int i = tu_data->retry_pages_num++;
    tu_data->retry_pages[i].vp = virt_page;
    tu_data->retry_pages[i].pp = phys_page;
    return true;
}

/* Add an unprotected physical page to the deduplicated no-lock protect set. */
#ifdef CONFIG_SOFTMMU
static bool tu_pending_protect_record(uint64_t pp)
{
    uint64_t page = tu_page(pp);

    for (int i = 0; i < tu_data->pending_protect_num; i++) {
        if (tu_data->pending_protect_pages[i] == page) {
            return true;
        }
    }
    if (tu_data->pending_protect_num >= MAX_PAGE_IN_TU) {
        return false;
    }
    tu_data->pending_protect_pages[tu_data->pending_protect_num++] = page;
    return true;
}
#endif

/*
 * Acquire the PageDesc lock for @pp while preserving ascending lock order.
 * TU_LOCK_NEED_PROTECT leaves any locks already acquired to the caller, which
 * must release all of them before calling tlb_protect_code().
 */
int tu_lock_page(target_ulong vp, uint64_t pp, bool *waited)
{
    uint64_t page = tu_page(pp);
    int i;

    if (waited) {
        *waited = false;
    }
#ifdef CONFIG_SOFTMMU
    if (!tlb_code_page_is_protected(page)) {
        if (!tu_retry_pages_record(vp, page) ||
            !tu_pending_protect_record(page)) {
            return TU_LOCK_FALLBACK;
        }
        return TU_LOCK_NEED_PROTECT;
    }
#endif
    if (!tu_pages_record(vp, page)) {
        return TU_LOCK_FALLBACK;
    }

    i = tu_locked_pages_find(page);
    if (i >= 0) {
        return TU_LOCK_ALREADY_HELD;
    }

    if (tu_data->locked_pages_num == 0) {
        if (tb_trylock_page0(page)) {
            tb_lock_page0(page);
            if (waited) {
                *waited = true;
            }
        }
    } else if (page >= tu_locked_pages_max()) {
        tb_lock_page0(page);
    } else if (tb_trylock_page0(page)) {
        return TU_LOCK_RESTART_LOCK_ORDER;
    }

    assert(tu_data->locked_pages_num < MAX_PAGE_IN_TU);
    tu_data->locked_pages[tu_data->locked_pages_num++] = page;
#ifdef CONFIG_SOFTMMU
    if (!tlb_code_page_is_protected(page)) {
        if (!tu_retry_pages_record(vp, page) ||
            !tu_pending_protect_record(page)) {
            return TU_LOCK_FALLBACK;
        }
        return TU_LOCK_NEED_PROTECT;
    }
#endif
    return TU_LOCK_OK;
}

/* Release every PageDesc lock held by the current TU in reverse order. */
static void tu_unlock_pages(void)
{
    for (int i = tu_data->locked_pages_num - 1; i >= 0; i--) {
        tb_unlock_page0(tu_data->locked_pages[i]);
        tu_data->locked_pages[i] = -1;
    }
    tu_data->locked_pages_num = 0;
}

/*
 * Clear page state for a new explore attempt. The caller must have released
 * all PageDesc locks before invoking this function.
 */
void tu_page_lock_reset_explore(void)
{
    tu_data->pages_num = 0;
    tu_data->locked_pages_num = 0;
    memset(tu_data->pages, 0, sizeof(tu_data->pages));
    memset(tu_data->locked_pages, -1, sizeof(tu_data->locked_pages));
}

/* Reset page-lock retry state once at the start of tu_gen_code(). */
void tu_page_lock_reset_gencode(void)
{
    tu_data->lock_order_retry_count = 0;
    tu_data->retry_pages_num = 0;
    tu_data->pending_protect_num = 0;
    tu_data->protect_retry_count = 0;
    tu_data->disable_cross_page_for_run = false;
    memset(tu_data->retry_pages, 0, sizeof(tu_data->retry_pages));
    memset(tu_data->pending_protect_pages, -1,
           sizeof(tu_data->pending_protect_pages));
}

/* Copy all resolved explore mappings into the state kept across a restart. */
bool tu_page_lock_snapshot_retry(void)
{
    for (int i = 0; i < tu_data->pages_num; i++) {
        if (tu_data->pages[i].pp == -1) {
            continue;
        }
        if (!tu_retry_pages_record(tu_data->pages[i].vp,
                                   tu_data->pages[i].pp)) {
            return false;
        }
    }
    return true;
}

/* Compare physical page addresses for deterministic ascending lock order. */
static int tu_phys_page_cmp(const void *ap, const void *bp)
{
    uint64_t a = *(const uint64_t *)ap;
    uint64_t b = *(const uint64_t *)bp;

    return a < b ? -1 : a > b;
}

/*
 * Re-translate every saved virtual page without holding PageDesc locks and
 * collect pages that still require HAMT code protection.
 */
static bool tu_refresh_retry_pages(CPUArchState *env)
{
    tu_data->pending_protect_num = 0;
    for (int i = 0; i < tu_data->retry_pages_num; i++) {
        tb_page_addr_t pp = get_page_addr_code_hostp(
                env, tu_data->retry_pages[i].vp, NULL);

        if (pp == -1) {
            return false;
        }
        pp = tu_page(pp);
        tu_data->retry_pages[i].pp = pp;
#ifdef CONFIG_SOFTMMU
        if (!tlb_code_page_is_protected(pp) &&
            !tu_pending_protect_record(pp)) {
            return false;
        }
#endif
    }
    return true;
}

/*
 * Protect saved pages with no PageDesc locks held, then lock their current
 * physical mappings in ascending order. The final no-exit translation check
 * guarantees that the mappings and protection state stayed stable.
 */
bool tu_page_lock_prepare_retry(CPUState *cpu)
{
    CPUArchState *env = cpu->env_ptr;

    while (tu_data->protect_retry_count < TU_PROTECT_RETRY_MAX) {
        uint64_t phys_pages[MAX_PAGE_IN_TU];
        uint64_t expected_pages[MAX_PAGE_IN_TU];
        int phys_pages_num = 0;
        bool stable = true;

        g_assert(tu_data->locked_pages_num == 0);
        if (!tu_refresh_retry_pages(env)) {
            return false;
        }

#ifdef CONFIG_SOFTMMU
        if (tu_data->pending_protect_num) {
            for (int i = 0; i < tu_data->pending_protect_num; i++) {
                tlb_protect_code(tu_data->pending_protect_pages[i]);
            }
            tu_data->protect_retry_count++;
            continue;
        }
#endif

        for (int i = 0; i < tu_data->retry_pages_num; i++) {
            uint64_t pp = tu_data->retry_pages[i].pp;
            bool found = false;

            expected_pages[i] = pp;
            for (int j = 0; j < phys_pages_num; j++) {
                if (phys_pages[j] == pp) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                phys_pages[phys_pages_num++] = pp;
            }
        }
        qsort(phys_pages, phys_pages_num, sizeof(phys_pages[0]),
              tu_phys_page_cmp);

        for (int i = 0; i < phys_pages_num; i++) {
            tb_lock_page0(phys_pages[i]);
            tu_data->locked_pages[tu_data->locked_pages_num++] = phys_pages[i];
        }

        for (int i = 0; i < tu_data->retry_pages_num; i++) {
            tb_page_addr_t pp = get_page_addr_code_hostp_noexit(
                    env, tu_data->retry_pages[i].vp, NULL);

            if (pp == -1 || tu_page(pp) != expected_pages[i]
#ifdef CONFIG_SOFTMMU
                || !tlb_code_page_is_protected(expected_pages[i])
#endif
               ) {
                stable = false;
                if (pp != -1) {
                    tu_data->retry_pages[i].pp = tu_page(pp);
                }
            }
        }
        if (!stable) {
            tu_unlock_pages();
            tu_data->protect_retry_count++;
            continue;
        }

        for (int i = 0; i < tu_data->retry_pages_num; i++) {
            if (!tu_pages_record(tu_data->retry_pages[i].vp,
                                 tu_data->retry_pages[i].pp)) {
                tu_unlock_pages();
                return false;
            }
        }
        return true;
    }
    return false;
}

/* Account for a translated TB on each physical page that it intersects. */
void tu_pages_tb_add(TranslationBlock *tb)
{
    int i;

    if (!cp_tu_pages() || !tb) {
        return;
    }

    assert(tu_pages_record(tb->pc, tb_page_addr0(tb)));
    i = tu_pages_find(tb->pc);
    assert(i >= 0);
    tu_data->pages[i].tb_num++;

    if (tb_page_addr1(tb) != -1) {
        assert(tu_pages_record(tb->pc + TARGET_PAGE_SIZE,
                               tb_page_addr1(tb)));
        i = tu_pages_find(tb->pc + TARGET_PAGE_SIZE);
        assert(i >= 0);
        tu_data->pages[i].tb_num++;
    }
}

/* Initialize page accounting from the TU entry TB. */
void tu_pages_init(void)
{
    tu_pages_tb_add(tu_data->tb_list[0]);
}

/* Count a lock-order restart and disable cross-page explore at the limit. */
void tu_page_lock_note_order_retry(void)
{
    tu_data->lock_order_retry_count++;
    if (tu_data->lock_order_retry_count >= TU_LOCK_ORDER_RETRY_MAX) {
        tu_data->disable_cross_page_for_run = true;
    }
}

/* Entry point used by generic TB cleanup to release TU-owned page locks. */
void tb_unlock_tu_pages(void)
{
    tu_unlock_pages();
}

#endif
