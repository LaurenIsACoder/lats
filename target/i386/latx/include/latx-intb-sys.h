/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_INTB_SYS_H_
#define _LATX_INTB_SYS_H_

#define LATXS_INTB_LINK_ENABLE
#define LATXS_INTB_LINK_OPTION_CHECK





#ifdef LATXS_INTB_LINK_ENABLE

int gen_latxs_intb_lookup(void *code_ptr);
int gen_latxs_intb_lookup_compact(void *code_ptr);
void latxs_tr_gen_jmp_cache_hash(void *pc, void *tmp, void *hash);
void latxs_out_jmp_cache_hash(void *pc, void *tmp, void *hash);
void gen_latxs_intb_lookup_njc_legacy(void *code_ptr,
        void *tmp, void *tmp0, void *tmp1,
        void *label_lookup_miss, void *slot_out);
void latxs_tr_gen_intb_sync_gpc(void);
void gen_latxs_intb_lookup_njc_compact(void *code_ptr,
        void *tmp, void *tmp0, void *tmp1,
        void *label_lookup_miss, void *slot_out);
void gen_latxs_intb_lookup_njc_inline(bool compact);
int gen_latxs_intb_fsht_lookup(void *code_ptr, int fsht_id);
int gen_latxs_tu_cross_page_check(void *code_ptr, int n);
int gen_latxs_tu_cross_page_check_ptr(void *code_ptr);
int gen_latxs_intb_fsht_lookup_tucpfast(void *code_ptr, int fsht_id);

#ifdef LATXS_INTB_LINK_OPTION_CHECK
#define intb_link_enable()  __intb_link_enable()
int __intb_link_enable(void);
#else
#define intb_link_enable()  (1)
#endif

#else /* no LATXS_INTB_LINK_ENABLE */

#define intb_link_enable()  (0)

#endif /* LATXS_INTB_LINK_ENABLE */



#endif
