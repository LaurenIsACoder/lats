/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_TB_COUNTER_H_
#define _LATX_TB_COUNTER_H_

#ifdef CONFIG_LATX_PRINT

extern int option_tb_counter;

extern int tb_lookup_cnt[8];
extern int tb_jc_cnt[8];
extern int tb_htable_cnt[8];
extern int tb_aot_cnt[8];
extern int tb_trans_cnt[8];

#define tb_counter_print_before_tb() do {   \
    if (option_tb_counter) { \
        uint64_t ns = get_clock(); \
        static uint64_t so = 0; \
        static int cnt = 0; \
        uint64_t sn = ns / 1000000000; \
        if (!so) so = sn; \
        uint64_t soo = qatomic_cmpxchg(&so, sn, sn + 1); \
        if (soo == sn) { \
			cnt++; \
            printf("TB%03d CPL0 %08d/%08d/%08d/%08d/%08d  CPL3 %08d/%08d/%08d/%08d/%08d  \n",  \
			cnt, \
            tb_lookup_cnt[0]+tb_lookup_cnt[1]+tb_lookup_cnt[2]+tb_lookup_cnt[3], \
            tb_jc_cnt[0]+tb_jc_cnt[1]+tb_jc_cnt[2]+tb_jc_cnt[3], \
            tb_htable_cnt[0]+tb_htable_cnt[1]+tb_htable_cnt[2]+tb_htable_cnt[3], \
            tb_aot_cnt[0]+tb_aot_cnt[1]+tb_aot_cnt[2]+tb_aot_cnt[3], \
            tb_trans_cnt[0]+tb_trans_cnt[1]+tb_trans_cnt[2]+tb_trans_cnt[3], \
            tb_lookup_cnt[4]+tb_lookup_cnt[5]+tb_lookup_cnt[6]+tb_lookup_cnt[7], \
                tb_jc_cnt[4]+    tb_jc_cnt[5]+    tb_jc_cnt[6]+    tb_jc_cnt[7], \
            tb_htable_cnt[4]+tb_htable_cnt[5]+tb_htable_cnt[6]+tb_htable_cnt[7], \
               tb_aot_cnt[4]+   tb_aot_cnt[5]+   tb_aot_cnt[6]+   tb_aot_cnt[7], \
             tb_trans_cnt[4]+ tb_trans_cnt[5]+ tb_trans_cnt[6]+ tb_trans_cnt[7] \
			); \
			for(int i=0;i<8;i++) { \
            tb_lookup_cnt[i] = 0; \
            tb_jc_cnt[i] = 0; \
            tb_htable_cnt[i] = 0; \
            tb_aot_cnt[i] = 0; \
            tb_trans_cnt[i] = 0; \
			} \
        } \
    } \
} while (0)

#endif

#endif
