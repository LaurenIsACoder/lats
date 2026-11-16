/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stdint.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "sysemu/rr.h"

struct {
    uint64_t helper_ld_nr;
    uint64_t helper_st_nr;
    uint64_t stlb_ld_nr;
    uint64_t stlb_ld_miss_nr;
    uint64_t stlb_st_nr;
    uint64_t stlb_st_miss_nr;
    uint64_t stlb_fld_nr;
    uint64_t stlb_fld_miss_nr;
    uint64_t stlb_fst_nr;
    uint64_t stlb_fst_miss_nr;
    uint64_t stlb_vld_nr;
    uint64_t stlb_vld_miss_nr;
    uint64_t stlb_vst_nr;
    uint64_t stlb_vst_miss_nr;
    uint64_t pf_cpl0_nr;
    uint64_t pf_cpl3_nr;
    uint64_t pg_mode_zero_nr;
    uint64_t pg_mode_pae_nr;
    uint64_t pg_mode_pae_lma_nr;
    uint64_t pg_mode_pae_lma_la57_nr;
} rrdata;

uint64_t *rr_ptr;

static void __attribute__((__constructor__)) rr_counter_init(void)
{
    rr_ptr = (void *)&rrdata;
    rr_counter_reset_all();
}

void *rr_counter_get_ptr(void)
{
    assert(rr_ptr != NULL);
    return rr_ptr;
}



uint64_t rr_counter_get(int event)
{
    return rr_ptr[event];
}

void rr_counter_record(int event)
{
    rr_ptr[event] += 1;
}

void rr_counter_set(int event, uint64_t val)
{
    rr_ptr[event] = val;
}

void rr_counter_reset(int event)
{
    rr_ptr[event] = 0;
}



void rr_counter_reset_all(void)
{
    memset(rr_ptr, 0, sizeof(rrdata));
}

void rr_counter_dump_all(void *fd)
{
    fprintf(fd, "%ld ", rrdata.helper_ld_nr);
    fprintf(fd, "%ld ", rrdata.helper_st_nr);

    fprintf(fd, "%ld ", rrdata.stlb_ld_nr);
    fprintf(fd, "%ld ", rrdata.stlb_ld_miss_nr);
    fprintf(fd, "%ld ", rrdata.stlb_st_nr);
    fprintf(fd, "%ld ", rrdata.stlb_st_miss_nr);

    fprintf(fd, "%ld ", rrdata.stlb_fld_nr);
    fprintf(fd, "%ld ", rrdata.stlb_fld_miss_nr);
    fprintf(fd, "%ld ", rrdata.stlb_fst_nr);
    fprintf(fd, "%ld ", rrdata.stlb_fst_miss_nr);

    fprintf(fd, "%ld ", rrdata.stlb_vld_nr);
    fprintf(fd, "%ld ", rrdata.stlb_vld_miss_nr);
    fprintf(fd, "%ld ", rrdata.stlb_vst_nr);
    fprintf(fd, "%ld ", rrdata.stlb_vst_miss_nr);

    fprintf(fd, "%ld ", rrdata.pf_cpl0_nr);
    fprintf(fd, "%ld ", rrdata.pf_cpl3_nr);

    fprintf(fd, "%ld ", rrdata.pg_mode_zero_nr);
    fprintf(fd, "%ld ", rrdata.pg_mode_pae_nr);
    fprintf(fd, "%ld ", rrdata.pg_mode_pae_lma_nr);
    fprintf(fd, "%ld ", rrdata.pg_mode_pae_lma_la57_nr);
}
