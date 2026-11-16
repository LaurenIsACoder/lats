/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HATM_ASM_H_
#define _HATM_ASM_H_

#include "hamt-data.h"

.macro hamt_tlbrefill_dbt ehi tmp
    srli.d  \ehi,  \ehi, (TLB_PS_1G_shift + 1)
    slli.d  \ehi,  \ehi, (TLB_PS_1G_shift + 1)
    ori     \tmp,  zero, TLB_PS_1G
    add.d   \tmp,  \tmp, \ehi
    csrwr   \tmp,  LOONGARCH_CSR_TLBREHI
    csrrd   \tmp,  LOONGARCH_CSR_KS6
    add.d   \tmp,  \tmp, \ehi
    csrwr   \tmp,  LOONGARCH_CSR_TLBRELO0
    csrrd   \tmp,  LOONGARCH_CSR_KS7
    add.d   \tmp,  \tmp, \ehi
    csrwr   \tmp,  LOONGARCH_CSR_TLBRELO1
    tlbfill
.endm

.macro hamt_tlbrefill_guest_zero tmp
    srli.d  \tmp,  \tmp, 0x6
    slli.d  \tmp,  \tmp, 0x6
    ori     \tmp,  \tmp, TLB_PS_4K
    csrwr   \tmp,  LOONGARCH_CSR_TLBREHI
    csrwr   zero,  LOONGARCH_CSR_TLBRELO0
    csrwr   zero,  LOONGARCH_CSR_TLBRELO1
    tlbfill
.endm

.macro hamt_tlbrefill_load_guest_mmuidx env tmp0 tmp1 tmp2
    ldptr.d  \tmp0,  \env,   ENV32_HFLAGS_OFFSET // hflags
    andi     \tmp1,  \tmp0,  0x3
    bnez     \tmp1,  1f
    srli.d   \tmp1,  \tmp0,  23 // HF_SMAP_MASK
    andi     \tmp1,  \tmp1,  0x1
    ld.wu    \tmp2,  \env,   ENV32_EFLAGS_OFFSET // eflags
    srli.d   \tmp2,  \tmp2,  18 // AC_MASK
    andi     \tmp2,  \tmp2,  0x1
    // tmp1 == 1 and tmp2 == 0 : KSMAP_IDX (0)
    xor      \tmp2,  \tmp1,  \tmp2 // 1
    and      \tmp2,  \tmp1,  \tmp2 // 1
    ori      \tmp1,  zero,   2 // MMU_KNOSMAP_IDX (2)
    maskeqz  \tmp2,  \tmp1,  \tmp2
    b        2f
1:
    ori \tmp2, zero, 1
2:
.endm

.macro hamt_tlbrefill_guest32_pgd spt
    lddir   \spt, \spt, 1
    ldpte   \spt, 0
    ldpte   \spt, 1
    tlbfill
.endm

.macro hamt_tlbrefill_guest64_pgd spt
    lddir   \spt, \spt, 3
    lddir   \spt, \spt, 2
    lddir   \spt, \spt, 1
    ldpte   \spt, 0
    ldpte   \spt, 1
    tlbfill
.endm



.macro hamt_load_data_base_s0 base
    or       \base,  zero, zero
    lu32i.d  \base,  0x8100000000>>32
    lu52i.d  \base,  \base, 0x9000000000000000>>52
.endm

.macro hamt_load_data_base_s1 base
    or       \base,  zero, zero
    lu32i.d  \base,  0x8200000000>>32
    lu52i.d  \base,  \base, 0x9000000000000000>>52
.endm

.macro hamt_load_data_base_s2 base
    or       \base,  zero, zero
    lu32i.d  \base,  0x8300000000>>32
    lu52i.d  \base,  \base, 0x9000000000000000>>52
.endm

.macro hamt_load_data_base_s3 base
    or       \base,  zero, zero
    lu32i.d  \base,  0x8400000000>>32
    lu52i.d  \base,  \base, 0x9000000000000000>>52
.endm



.macro hamt_load_code_base_s0 base
    lu12i.w  \base,  0x0
    lu32i.d  \base,  0x8000000000>>32
.endm

.macro hamt_load_code_base_s1 base
    lu12i.w  \base,  0x4
    lu32i.d  \base,  0x8000000000>>32
.endm

.macro hamt_load_code_base_s2 base
    lu12i.w  \base,  0x8
    lu32i.d  \base,  0x8000000000>>32
.endm

.macro hamt_load_code_base_s3 base
    lu12i.w  \base,  0xc
    lu32i.d  \base,  0x8000000000>>32
.endm



.macro hamt_load_code_base_fpe_s0 base
    lu12i.w  \base,  0x10
    lu32i.d  \base,  0x8000000000>>32
.endm

.macro hamt_load_code_base_fpe_s1 base
    lu12i.w  \base,  0x14
    lu32i.d  \base,  0x8000000000>>32
.endm

.macro hamt_load_code_base_fpe_s2 base
    lu12i.w  \base,  0x18
    lu32i.d  \base,  0x8000000000>>32
.endm

.macro hamt_load_code_base_fpe_s3 base
    lu12i.w  \base,  0x1c
    lu32i.d  \base,  0x8000000000>>32
.endm



.macro hamt_load_err_data_base_s0 base
    lu12i.w  \base,  0x10
    lu32i.d  \base,  0x8100000000>>32
    lu52i.d  \base,  \base, 0x9000000000000000>>52
.endm

.macro hamt_load_err_data_base_s1 base
    lu12i.w  \base,  0x10
    lu32i.d  \base,  0x8200000000>>32
    lu52i.d  \base,  \base, 0x9000000000000000>>52
.endm

.macro hamt_load_err_data_base_s2 base
    lu12i.w  \base,  0x10
    lu32i.d  \base,  0x8300000000>>32
    lu52i.d  \base,  \base, 0x9000000000000000>>52
.endm

.macro hamt_load_err_data_base_s3 base
    lu12i.w  \base,  0x10
    lu32i.d  \base,  0x8400000000>>32
    lu52i.d  \base,  \base, 0x9000000000000000>>52
.endm



.macro hamt_load_err_code_base_s0 base
    lu12i.w  \base,  0x20
    lu32i.d  \base,  0x8100000000>>32
.endm

.macro hamt_load_err_code_base_s1 base
    lu12i.w  \base,  0x20
    lu32i.d  \base,  0x8200000000>>32
.endm

.macro hamt_load_err_code_base_s2 base
    lu12i.w  \base,  0x20
    lu32i.d  \base,  0x8300000000>>32
.endm

.macro hamt_load_err_code_base_s3 base
    lu12i.w  \base,  0x20
    lu32i.d  \base,  0x8400000000>>32
.endm






#ifdef BG_COUNTER_GROUP_HAMT
.macro hamt_counter_load_base base
    or       \base,  zero, zero
    lu32i.d  \base,  0x8100000000>>32
    lu52i.d  \base,  \base, 0x9000000000000000>>52
.endm

.macro hamt_counter_load_base_s0 base
    hamt_load_data_base_s0 \base
.endm

.macro hamt_counter_load_base_s1 base
    hamt_load_data_base_s1 \base
.endm

.macro hamt_counter_load_base_s2 base
    hamt_load_data_base_s2 \base
.endm

.macro hamt_counter_load_base_s3 base
    hamt_load_data_base_s3 \base
.endm

.macro hamt_counter_tlbr_dbt tmp1 base
    ld.d     \tmp1,  \base,  (INDEX_TLBR_DBT * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBR_DBT * 8)
.endm

.macro hamt_counter_tlbr_guest tmp1 base env
    ld.d     \tmp1,  \base,  (INDEX_TLBR_GUEST * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBR_GUEST * 8)
    ldptr.d  \tmp1,  \env,   ENV32_HFLAGS_OFFSET // hflags
    andi     \tmp1,  \tmp1,  0x3
    beqz     \tmp1,  1f
    // CPL3 : mmuindex = 1
    ld.d     \tmp1,  \base,  (INDEX_TLBR_GUEST_MMU1 * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBR_GUEST_MMU1 * 8)
    b 2f
1:
    // CPL0 : mmuindex = 2
    ld.d     \tmp1,  \base,  (INDEX_TLBR_GUEST_MMU2 * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBR_GUEST_MMU2 * 8)
2:
.endm

.macro hamt_counter_tlbl_guest tmp1 base env
    ld.d     \tmp1,  \base,  (INDEX_TLBL_GUEST * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBL_GUEST * 8)
    or       \tmp1,  \env,   zero
    lu52i.d  \tmp1,  \tmp1,  0x9000000000000000>>52
    ldptr.d  \tmp1,  \tmp1,  ENV32_HFLAGS_OFFSET // hflags
    andi     \tmp1,  \tmp1,  0x3
    beqz     \tmp1,  1f
    // CPL3 : mmuindex = 1
    ld.d     \tmp1,  \base,  (INDEX_TLBL_GUEST_MMU1 * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBL_GUEST_MMU1 * 8)
    b 2f
1:
    // CPL0 : mmuindex = 2
    ld.d     \tmp1,  \base,  (INDEX_TLBL_GUEST_MMU2 * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBL_GUEST_MMU2 * 8)
2:
.endm

.macro hamt_counter_tlbs_guest tmp1 base env
    ld.d     \tmp1,  \base,  (INDEX_TLBS_GUEST * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBS_GUEST * 8)
    or       \tmp1,  \env,   zero
    lu52i.d  \tmp1,  \tmp1,  0x9000000000000000>>52
    ldptr.d  \tmp1,  \tmp1,  ENV32_HFLAGS_OFFSET // hflags
    andi     \tmp1,  \tmp1,  0x3
    beqz     \tmp1,  1f
    // CPL3 : mmuindex = 1
    ld.d     \tmp1,  \base,  (INDEX_TLBS_GUEST_MMU1 * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBS_GUEST_MMU1 * 8)
    b 2f
1:
    // CPL0 : mmuindex = 2
    ld.d     \tmp1,  \base,  (INDEX_TLBS_GUEST_MMU2 * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBS_GUEST_MMU2 * 8)
2:
.endm

.macro hamt_counter_tlbm_guest tmp1 base env
    ld.d     \tmp1,  \base,  (INDEX_TLBM_GUEST * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBM_GUEST * 8)
    or       \tmp1,  \env,   zero
    lu52i.d  \tmp1,  \tmp1,  0x9000000000000000>>52
    ldptr.d  \tmp1,  \tmp1,  ENV32_HFLAGS_OFFSET // hflags
    andi     \tmp1,  \tmp1,  0x3
    beqz     \tmp1,  1f
    // CPL3 : mmuindex = 1
    ld.d     \tmp1,  \base,  (INDEX_TLBM_GUEST_MMU1 * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBM_GUEST_MMU1 * 8)
    b 2f
1:
    // CPL0 : mmuindex = 2
    ld.d     \tmp1,  \base,  (INDEX_TLBM_GUEST_MMU2 * 8)
    addi.d   \tmp1,  \tmp1,  1
    st.d     \tmp1,  \base,  (INDEX_TLBM_GUEST_MMU2 * 8)
2:
.endm

#else
.macro hamt_counter_load_base base
.endm
.macro hamt_counter_tlbr_dbt tmp1 base
.endm
.macro hamt_counter_tlbr_guest tmp1 base env
.endm
.macro hamt_counter_tlbl_guest tmp1 base env
.endm
.macro hamt_counter_tlbs_guest tmp1 base env
.endm
.macro hamt_counter_tlbm_guest tmp1 base env
.endm
.macro hamt_counter_load_base_s0 base
.endm
.macro hamt_counter_load_base_s1 base
.endm
.macro hamt_counter_load_base_s2 base
.endm
.macro hamt_counter_load_base_s3 base
.endm

#endif



#endif
