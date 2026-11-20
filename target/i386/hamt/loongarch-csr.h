/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LOONGARCH_CSR_H_
#define _LOONGARCH_CSR_H_

#define LOONGARCH_CSR_CRMD      0x0
#define  CSR_CRMD_PG_SHIFT      4
#define  CSR_CRMD_PG            ((0x1) << CSR_CRMD_PG_SHIFT)
#define  CSR_CRMD_DA_SHIFT      3
#define  CSR_CRMD_DA            ((0x1) << CSR_CRMD_DA_SHIFT)
#define  CSR_CRMD_IE_SHIFT      2
#define  CSR_CRMD_IE            ((0x1) << CSR_CRMD_IE_SHIFT)

#define LOONGARCH_CSR_ECFG      0x4
#define LOONGARCH_CSR_PRMD      0x1

/* TLB refill registers */
#define LOONGARCH_CSR_TLBREBASE 0x88 /* TLB refill exception base address */
#define LOONGARCH_CSR_TLBRBADV  0x89 /* TLB refill badvaddr */
#define LOONGARCH_CSR_TLBREPC   0x8a /* TLB refill EPC */
#define LOONGARCH_CSR_TLBRSAVE  0x8b /* KScratch for TLB refill exception */
#define LOONGARCH_CSR_TLBRELO0  0x8c /* TLB refill entrylo0 */
#define LOONGARCH_CSR_TLBRELO1  0x8d /* TLB refill entrylo1 */
#define LOONGARCH_CSR_TLBREHI   0x8e /* TLB refill entryhi */
#define LOONGARCH_CSR_TLBRPRMD  0x8f /* TLB refill mode info */
#define LOONGARCH_CSR_ASID      0x18 /* ASID */
#define LOONGARCH_CSR_ESTAT     0x5  /* ESTAT */
#define LOONGARCH_CSR_EPC       0x6  /* EPC */
#define LOONGARCH_CSR_BADV      0x7  /* Bad virtual address */
#define LOONGARCH_CSR_BADI      0x8  /* Bad virtual address */
#define LOONGARCH_CSR_TLBEHI    0x11 /* TLB EntryHi */
#define LOONGARCH_CSR_TLBIDX    0x10
#define LOONGARCH_CSR_TLBELO0   0x12
#define LOONGARCH_CSR_TLBELO1   0x13

/* Kscratch registers */
#define LOONGARCH_CSR_KS0 0x30
#define LOONGARCH_CSR_KS1 0x31
#define LOONGARCH_CSR_KS2 0x32
#define LOONGARCH_CSR_KS3 0x33
#define LOONGARCH_CSR_KS4 0x34
#define LOONGARCH_CSR_KS5 0x35
#define LOONGARCH_CSR_KS6 0x36
#define LOONGARCH_CSR_KS7 0x37
#define LOONGARCH_CSR_KS8 0x38

/* Timer registers */
#define LOONGARCH_CSR_TIMERID     0x40

#define LOONGARCH_CSR_TIMERCFG    0x41
#define  CSR_TCFG_VAL_SHIFT       2
#define  CSR_TCFG_VAL             (0x3fffffffffffUL << CSR_TCFG_VAL_SHIFT)
#define  CSR_TCFG_PERIOD_SHIFT    1
#define  CSR_TCFG_PERIOD          (0x1UL << CSR_TCFG_PERIOD_SHIFT)
#define  CSR_TCFG_EN              (0x1)

#define LOONGARCH_CSR_TIMERTICK   0x42

#define LOONGARCH_CSR_TIMEROFFSET 0x43

#define LOONGARCH_CSR_TINTCLR     0x44
#define  CSR_TINTCLR_TI_SHIFT     0
#define  CSR_TINTCLR_TI           (1 << CSR_TINTCLR_TI_SHIFT)

#endif
