/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _RR_H_
#define _RR_H_

#include "qemu/osdep.h"

#ifdef CONFIG_LATX_RR
#define RR_COUNTER_ENABLE
#endif


#define RR_EVENT_HELPER_LD  0
#define RR_EVENT_HELPER_ST  1

#define RR_EVENT_STLB_LD        2
#define RR_EVENT_STLB_LD_MISS   3
#define RR_EVENT_STLB_ST        4
#define RR_EVENT_STLB_ST_MISS   5

#define RR_EVENT_STLB_FLD        6
#define RR_EVENT_STLB_FLD_MISS   7
#define RR_EVENT_STLB_FST        8
#define RR_EVENT_STLB_FST_MISS   9

#define RR_EVENT_STLB_VLD        10
#define RR_EVENT_STLB_VLD_MISS   11
#define RR_EVENT_STLB_VST        12
#define RR_EVENT_STLB_VST_MISS   13

#define RR_EVENT_PF_CPL0    14
#define RR_EVENT_PF_CPL3    15

#define RR_EVENT_PGMODE_ZERO            16
#define RR_EVENT_PGMODE_PAE             17
#define RR_EVENT_PGMODE_PAE_LMA         18
#define RR_EVENT_PGMODE_PAE_LMA_LA57    19

#ifdef RR_COUNTER_ENABLE

#define rr_counter_enabled()    (1)
void *rr_counter_get_ptr(void);

static inline int rr_offsetof(int event) {
    return sizeof(uint64_t) * event;
}

uint64_t rr_counter_get(int event);
void rr_counter_record(int event);
void rr_counter_set(int event, uint64_t val);
void rr_counter_reset(int event);

void rr_counter_reset_all(void);
void rr_counter_dump_all(void *fd);

#else

#define rr_counter_enabled()    (0)
#define rr_counter_get_ptr()    (NULL)

#define rr_offsetof(event)  (0)

#define rr_counter_get(n)   ((uint64_t)0)
#define rr_counter_record(a)
#define rr_counter_set(id, val)

#define rr_counter_reset_all()
#define rr_counter_dump_all(fd)

#endif

#endif
