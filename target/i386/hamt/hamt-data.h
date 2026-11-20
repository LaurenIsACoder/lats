/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_DATA_H_
#define _HAMT_DATA_H_

#define PT_ERA_INDEX       32
#define PT_BADV_INDEX      33
#define PT_CRMD_INDEX      34
#define PT_PRMD_INDEX      35
#define PT_EUEN_INDEX      36
#define PT_ECFG_INDEX      37
#define PT_ESTAT_INDEX     38
#define PT_ASID_INDEX      39

#define HD_TRAP_INDEX       50
#define HD_ERRCODE_INDEX    51
#define HD_ERRDATA1_INDEX   52
#define HD_ERRDATA2_INDEX   53
#define HD_ERRDATA3_INDEX   54
#define HD_ERRDATA4_INDEX   55
#define HD_ERRDATA5_INDEX   56
#define HD_ERRDATA6_INDEX   57
#define HD_ERRDATA7_INDEX   58
#define HD_ERRDATA8_INDEX   59

#define HD_CNT_IPI_INDEX    80
#define HD_CNT_IPIOLD_INDEX 81
#define HD_CNT_TLBR_DBT_INDEX   82
#define HD_CNT_TLBR_X86_INDEX   83
#define HD_CNT_TLBR_X86_2M_INDEX    84
#define HD_CNT_TLBR_X86_4K_INDEX    85
#define HD_CNT_TLBR_X86_OK_INDEX    86
#define HD_CNT_TLBR_X86_XX_INDEX    87
#define HD_CNT_TLBR_X86_C0_INDEX    88
#define HD_CNT_TLBR_X86_C3_INDEX    89
#define HD_CNT_TLBR_X86_OK1M_INDEX  90
#define HD_CNT_TLBR_X86_OK1M4K_INDEX  91

#define HD_TLBR_RDTIME_ST   100
#define HD_TLBR_RDTIME_ALL  101

#define HD_TLBELO1_INDEX   508
#define HD_TLBELO0_INDEX   509
#define HD_TLBEHI_INDEX    510
#define HD_TLBIDX_INDEX    511

#define TLB_PS_4K 0xc
#define TLB_PS_1G 0x1e
#define TLB_PS_1G_shift 30
#define TLB_MASK (1 << TLB_PS - 1)

#define PT_R0 0 /* offsetof(struct pt_regs, regs[0]) */
#define PT_R1 8 /* offsetof(struct pt_regs, regs[1]) */
#define PT_R2 16 /* offsetof(struct pt_regs, regs[2]) */
#define PT_R3 24 /* offsetof(struct pt_regs, regs[3]) */
#define PT_R4 32 /* offsetof(struct pt_regs, regs[4]) */
#define PT_R5 40 /* offsetof(struct pt_regs, regs[5]) */
#define PT_R6 48 /* offsetof(struct pt_regs, regs[6]) */
#define PT_R7 56 /* offsetof(struct pt_regs, regs[7]) */
#define PT_R8 64 /* offsetof(struct pt_regs, regs[8]) */
#define PT_R9 72 /* offsetof(struct pt_regs, regs[9]) */
#define PT_R10 80 /* offsetof(struct pt_regs, regs[10]) */
#define PT_R11 88 /* offsetof(struct pt_regs, regs[11]) */
#define PT_R12 96 /* offsetof(struct pt_regs, regs[12]) */
#define PT_R13 104 /* offsetof(struct pt_regs, regs[13]) */
#define PT_R14 112 /* offsetof(struct pt_regs, regs[14]) */
#define PT_R15 120 /* offsetof(struct pt_regs, regs[15]) */
#define PT_R16 128 /* offsetof(struct pt_regs, regs[16]) */
#define PT_R17 136 /* offsetof(struct pt_regs, regs[17]) */
#define PT_R18 144 /* offsetof(struct pt_regs, regs[18]) */
#define PT_R19 152 /* offsetof(struct pt_regs, regs[19]) */
#define PT_R20 160 /* offsetof(struct pt_regs, regs[20]) */
#define PT_R21 168 /* offsetof(struct pt_regs, regs[21]) */
#define PT_R22 176 /* offsetof(struct pt_regs, regs[22]) */
#define PT_R23 184 /* offsetof(struct pt_regs, regs[23]) */
#define PT_R24 192 /* offsetof(struct pt_regs, regs[24]) */
#define PT_R25 200 /* offsetof(struct pt_regs, regs[25]) */
#define PT_R26 208 /* offsetof(struct pt_regs, regs[26]) */
#define PT_R27 216 /* offsetof(struct pt_regs, regs[27]) */
#define PT_R28 224 /* offsetof(struct pt_regs, regs[28]) */
#define PT_R29 232 /* offsetof(struct pt_regs, regs[29]) */
#define PT_R30 240 /* offsetof(struct pt_regs, regs[30]) */
#define PT_R31 248 /* offsetof(struct pt_regs, regs[31]) */
#define PT_ERA 256 /* offsetof(struct pt_regs, csr_era) */
#define PT_BVADDR 264 /* offsetof(struct pt_regs, csr_badv) */
#define PT_CRMD 272 /* offsetof(struct pt_regs, csr_crmd) */
#define PT_PRMD 280 /* offsetof(struct pt_regs, csr_prmd) */
#define PT_EUEN 288 /* offsetof(struct pt_regs, csr_euen) */
#define PT_ECFG 296 /* offsetof(struct pt_regs, csr_ecfg) */
#define PT_ESTAT 304 /* offsetof(struct pt_regs, csr_estat) */
#define PT_ASID 312 /* offsetof(struct pt_regs, csr_estat) */
#endif
