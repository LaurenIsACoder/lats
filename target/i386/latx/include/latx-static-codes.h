/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_STATIC_CODES_H_
#define _LATX_STATIC_CODES_H_

#include "latx-shadow-htable.h"

typedef struct latx_sc_table_t {

    uint64_t intb_lookup;
    uint64_t intb_lookup_compact;

    uint64_t scs_prologue;
    uint64_t scs_prologue_nofp;
    uint64_t scs_epilogue;
    uint64_t scs_epilogue_nofp;

    uint64_t cs_bt_to_native;
    uint64_t cs_native_to_bt_ret_0;
    uint64_t cs_native_to_bt_ret_n3;
    uint64_t cs_native_to_bt_ret_n1;
    uint64_t cs_native_to_bt_ret_n0;
    //uint64_t cs_native_to_bt;

    uint64_t cp_check_n0_p01_mmu_user,cp_check_n0_p01_mmu_user_intcheck;
    uint64_t cp_check_n1_p01_mmu_user,cp_check_n1_p01_mmu_user_intcheck;
    uint64_t cp_check_n0_p01_mmu_knosmap,cp_check_n0_p01_mmu_knosmap_intcheck;
    uint64_t cp_check_n1_p01_mmu_knosmap,cp_check_n1_p01_mmu_knosmap_intcheck;
    uint64_t cp_check_n0_p0_mmu_user,cp_check_n0_p0_mmu_user_intcheck;
    uint64_t cp_check_n1_p0_mmu_user,cp_check_n1_p0_mmu_user_intcheck;
    uint64_t cp_check_n0_p0_mmu_knosmap,cp_check_n0_p0_mmu_knosmap_intcheck;
    uint64_t cp_check_n1_p0_mmu_knosmap,cp_check_n1_p0_mmu_knosmap_intcheck;
    uint64_t cp_check_n0_p1_mmu_user,cp_check_n0_p1_mmu_user_intcheck;
    uint64_t cp_check_n1_p1_mmu_user,cp_check_n1_p1_mmu_user_intcheck;
    uint64_t cp_check_n0_p1_mmu_knosmap,cp_check_n0_p1_mmu_knosmap_intcheck;
    uint64_t cp_check_n1_p1_mmu_knosmap,cp_check_n1_p1_mmu_knosmap_intcheck;

    uint64_t cp_check_ptw_n0_p01_mmu_user,cp_check_ptw_n0_p01_mmu_user_intcheck;
    uint64_t cp_check_ptw_n1_p01_mmu_user,cp_check_ptw_n1_p01_mmu_user_intcheck;
    uint64_t cp_check_ptw_n0_p0_mmu_user,cp_check_ptw_n0_p0_mmu_user_intcheck;
    uint64_t cp_check_ptw_n1_p0_mmu_user,cp_check_ptw_n1_p0_mmu_user_intcheck;
    uint64_t cp_check_ptw_n0_p1_mmu_user,cp_check_ptw_n0_p1_mmu_user_intcheck;
    uint64_t cp_check_ptw_n1_p1_mmu_user,cp_check_ptw_n1_p1_mmu_user_intcheck;

    uint64_t cp_check_ptw_n0_p01_mmu_knosmap,cp_check_ptw_n0_p01_mmu_knosmap_intcheck;
    uint64_t cp_check_ptw_n1_p01_mmu_knosmap,cp_check_ptw_n1_p01_mmu_knosmap_intcheck;
    uint64_t cp_check_ptw_n0_p0_mmu_knosmap,cp_check_ptw_n0_p0_mmu_knosmap_intcheck;
    uint64_t cp_check_ptw_n1_p0_mmu_knosmap,cp_check_ptw_n1_p0_mmu_knosmap_intcheck;
    uint64_t cp_check_ptw_n0_p1_mmu_knosmap,cp_check_ptw_n0_p1_mmu_knosmap_intcheck;
    uint64_t cp_check_ptw_n1_p1_mmu_knosmap,cp_check_ptw_n1_p1_mmu_knosmap_intcheck;

    uint64_t cp_check_ptw_user;
    uint64_t cp_check_ptw_knosmap;

    uint64_t tu_cp_check_n0_mmu_user, tu_cp_check_n0_mmu_user_intcheck;
    uint64_t tu_cp_check_n1_mmu_user, tu_cp_check_n1_mmu_user_intcheck;
    uint64_t tu_cp_check_ptr_mmu_user;

    uint64_t intb_fsht_lookup[32];
    uint64_t intb_fsht_after_njc[32];
    uint64_t intb_fsht_lookup_tucpfast[32];

    uint64_t ras_tp_fallback;
    uint64_t ras_tp_repair;
    /* Keep existing fields' offsets unchanged. JIT-only front ends retain
     * source-site information in the hardware indirect predictor's PC. */
    uint64_t intb_njc_return;
    uint64_t intb_fsht_call_lookup_tucpfast[32];
    uint64_t intb_fsht_direct_tail_lookup_tucpfast[32];
    uint64_t intb_resume_lookup;
    uint64_t aot_native_syscall;
    uint64_t aot_native_atomic;
    uint64_t aot_native_cpu_query;
    uint64_t aot_native_x87;
    uint64_t aot_native_system;
} latx_sc_table_t;

extern latx_sc_table_t *latx_sc_table;

#define SET_SC_TABLE(rid, name, value) do {     \
    latx_sc_table[rid].name = (value);          \
} while (0)

#define GET_SC_TABLE(rid, name) (latx_sc_table[rid].name)
#define GET_SC_TABLE_OFFSET(rid, name, offset) (latx_sc_table[rid].name + offset[rid])



#endif
