/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_CP_COUNTER_H_
#define _LATX_CP_COUNTER_H_

#ifdef CONFIG_LATX_PRINT

extern uint64_t cp_num[2];
extern uint64_t cp_tb_hit[2];
extern uint64_t cp_itlb_hit[2];
extern uint64_t cp_ptw_hit[2];
extern uint64_t cp_check_hit[2];
extern int option_cross_page_counter;

#define cp_counter_print_before_tb() do {   \
    if (option_cross_page_counter) { \
        uint64_t ns = get_clock();  \
        static uint64_t so = 0; \
        uint64_t sn = ns / 1000000000;  \
        if (!so) so = sn; \
        uint64_t soo = qatomic_cmpxchg(&so, sn, sn + 1); \
        if (soo == sn) { \
            printf("cross-page tblink : ALL/TB/iTLB/iTLBptw/HIT = " \
                    "CPL0 %8lu/%8lu(%3d)/%8lu(%3d)/%8lu(%3d)/%8lu(%3d)  " \
                    "CPL3 %8lu/%8lu(%3d)/%8lu(%3d)/%8lu(%3d)/%8lu(%3d)\n", \
                    cp_num[0], \
                    cp_tb_hit[0],    cp_num[0] ? (int)(cp_tb_hit[0]    * 100 / cp_num[0]) : 0, \
                    cp_itlb_hit[0],  cp_num[0] ? (int)(cp_itlb_hit[0]  * 100 / cp_num[0]) : 0, \
                    cp_ptw_hit[0],   cp_num[0] ? (int)(cp_ptw_hit[0]   * 100 / cp_num[0]) : 0, \
                    cp_check_hit[0], cp_num[0] ? (int)(cp_check_hit[0] * 100 / cp_num[0]) : 0, \
                    cp_num[1], \
                    cp_tb_hit[1],    cp_num[1] ? (int)(cp_tb_hit[1]    * 100 / cp_num[1]) : 0, \
                    cp_itlb_hit[1],  cp_num[1] ? (int)(cp_itlb_hit[1]  * 100 / cp_num[1]) : 0, \
                    cp_ptw_hit[1],   cp_num[1] ? (int)(cp_ptw_hit[1]   * 100 / cp_num[1]) : 0, \
                    cp_check_hit[1], cp_num[1] ? (int)(cp_check_hit[1] * 100 / cp_num[1]) : 0 \
                    ); \
            cp_num[0]       = 0; cp_num[1]       = 0; \
            cp_tb_hit[0]    = 0; cp_tb_hit[1]    = 0; \
            cp_itlb_hit[0]  = 0; cp_itlb_hit[1]  = 0; \
            cp_ptw_hit[0]   = 0; cp_ptw_hit[1]   = 0; \
            cp_check_hit[0] = 0; cp_check_hit[1] = 0; \
        } \
    } \
} while (0)

#define cp_counter_la_out_inc(t1, t2, v) do {       \
    if (option_cross_page_counter) { \
        la_imm(&(t1), (uint64_t)(&(v)));                \
        if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&t1); \
        la_out2i(LISA_ORI, &(t2), &la_zero, 1);         \
        la_out3(LISA_AMADD_DB_D, &la_zero, &t1, &t2);   \
    } \
} while (0)

#endif

#endif
