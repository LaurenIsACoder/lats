/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef LATX_TU_PAGE_LOCK_H
#define LATX_TU_PAGE_LOCK_H

#ifdef CONFIG_LATX_TU

#define TU_LOCK_ORDER_RETRY_MAX 4
#define TU_PROTECT_RETRY_MAX 8

#define TU_LOCK_OK                     0
#define TU_LOCK_ALREADY_HELD           1
#define TU_LOCK_RESTART_LOCK_ORDER     2
#define TU_LOCK_FALLBACK               3
#define TU_LOCK_NEED_PROTECT           4

#define TU_DECODE_NEED_PROTECT        -6

bool tu_cross_page_enabled(void);
int tu_pages_find(uint64_t vp);
bool tu_pages_append(uint64_t vp);
void tu_pages_tb_add(TranslationBlock *tb);
void tu_pages_init(void);
int tu_lock_page(target_ulong vp, uint64_t pp, bool *waited);
void tu_page_lock_reset_explore(void);
void tu_page_lock_reset_gencode(void);
bool tu_page_lock_snapshot_retry(void);
bool tu_page_lock_prepare_retry(CPUState *cpu);
void tu_page_lock_note_order_retry(void);
void tb_unlock_tu_pages(void);

#endif

#endif
