/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_INTB_COUNTER_H_
#define _LATX_INTB_COUNTER_H_

#ifdef CONFIG_LATX_PRINT

extern uint64_t fintb_num;
extern uint64_t fintb_1_njc_miss;
extern uint64_t fintb_1_njc_miss_zero;
extern uint64_t fintb_1_njc_hit;
extern uint64_t fintb_2_fsht_miss;
extern uint64_t fintb_2_fsht_miss_zero;
extern uint64_t fintb_2_fsht_hit;
extern uint64_t fintb_3_helper_miss;
extern uint64_t fintb_3_helper_hit;

extern uint64_t intb_num;
extern uint64_t intb_3_helper_miss;
extern uint64_t intb_3_helper_hit;

extern int option_intb_counter;

#define DIVINT(a, b) ((int)((b) ? (((a)*100)/(b)) : 0))

#define intb_counter_print_before_tb() do {   \
    if (option_intb_counter) { \
        uint64_t ns = get_clock();  \
        static uint64_t so = 0; \
        uint64_t sn = ns / 1000000000;  \
        if (!so) so = sn; \
        uint64_t soo = qatomic_cmpxchg(&so, sn, sn + 1); \
        if (soo == sn) { \
            uint64_t a0  = fintb_num; \
            uint64_t a1m = fintb_1_njc_miss; \
            uint64_t a1z = fintb_1_njc_miss_zero; \
            uint64_t a1h = fintb_1_njc_hit; \
            uint64_t a2m = fintb_2_fsht_miss; \
            uint64_t a2z = fintb_2_fsht_miss_zero; \
            uint64_t a2h = fintb_2_fsht_hit; \
            uint64_t a3m = fintb_3_helper_miss; \
            uint64_t a3h = fintb_3_helper_hit; \
            printf("%s:%d intb counter : " \
                    " FlagedLookup %8lu(%3d) /" \
                    " HIT:NJC,FSHT,HP = %3d,%3d,%3d /" \
                    " NJC %8lu(%8lu,%8lu) %3d(%3d,%3d) /" \
                    " FSHT %8lu(%8lu,%8lu) %3d(%3d,%3d) /" \
                    " Helper %8lu(%8lu) %3d" \
                    "\n", \
                    __func__, __LINE__, \
                    a0, DIVINT(a0, intb_num), \
                    DIVINT(a1h, a0), DIVINT(a2h, a0), DIVINT(a3h, a0), \
                    a1h, a1z, a1m-a1z, \
                    DIVINT(a1h, a0), DIVINT(a1z, a0), DIVINT(a1m-a1z, a0), \
                    a2h, a2z, a2m-a2z, \
                    DIVINT(a2h, a1m), DIVINT(a2z, a1m), DIVINT(a2m-a2z, a1m), \
                    a3h, a3m, DIVINT(a3h, a2m) \
                    ); \
            fintb_num              = 0; \
            fintb_1_njc_miss       = 0; \
            fintb_1_njc_miss_zero  = 0; \
            fintb_1_njc_hit        = 0; \
            fintb_2_fsht_miss      = 0; \
            fintb_2_fsht_miss_zero = 0; \
            fintb_2_fsht_hit       = 0; \
            fintb_3_helper_miss    = 0; \
            fintb_3_helper_hit     = 0; \
            intb_num           = 0; \
            intb_3_helper_miss = 0; \
            intb_3_helper_hit  = 0; \
        } \
    } \
} while (0)

#define intb_counter_la_out_inc(t1, t2, v) do {       \
    if (option_intb_counter) { \
        la_imm(&(t1), (uint64_t)(&(v)));                \
        if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&(t1));  \
        la_out2i(LISA_ORI, &(t2), &la_zero, 1);         \
        la_out3(LISA_AMADD_DB_D, &la_zero, &t1, &t2);   \
    } \
} while (0)

#endif

#endif
