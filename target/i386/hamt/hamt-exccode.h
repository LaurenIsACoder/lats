/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_EXCCODE_H_
#define _HAMT_EXCCODE_H_

#define EXCCODE_RSV         0   /* Reserved */
#define EXCCODE_TLBL        1   /* TLB miss on a load */
#define EXCCODE_TLBS        2   /* TLB miss on a store */
#define EXCCODE_TLBI        3   /* TLB miss on a ifetch */
#define EXCCODE_TLBM        4   /* TLB modified fault */
#define EXCCODE_TLBRI       5   /* TLB Read-Inhibit exception */
#define EXCCODE_TLBXI       6   /* TLB Execution-Inhibit exception */
#define EXCCODE_TLBPE       7   /* TLB Privilege Error */
#define EXCCODE_ADE         8   /* Address Error */
     #define EXSUBCODE_ADEF 0   /* Fetch Instruction */
     #define EXSUBCODE_ADEM 1   /* Access Memory*/
#define EXCCODE_ALE         9   /* Unalign Access */
#define EXCCODE_OOB         10  /* Out of bounds */
#define EXCCODE_SYS         11  /* System call */
#define EXCCODE_BP          12  /* Breakpoint */
#define EXCCODE_INE         13  /* Inst. Not Exist */
#define EXCCODE_IPE         14  /* Inst. Privileged Error */
#define EXCCODE_FPDIS       15  /* FPU Disabled */
#define EXCCODE_LSXDIS      16  /* LSX Disabled */
#define EXCCODE_LASXDIS     17  /* LASX Disabled */
#define EXCCODE_FPE         18  /* Floating Point Exception */
    #define EXCSUBCODE_FPE  0   /* Floating Point Exception */
    #define EXCSUBCODE_VFPE 1   /* Vector Exception */
#define EXCCODE_WATCH       19  /* Watch address reference */
#define EXCCODE_BTDIS       20  /* Binary Trans. Disabled */
#define EXCCODE_BTE         21  /* Binary Trans. Exception */
#define EXCCODE_PSI         22  /* Guest Privileged Error */
#define EXCCODE_HYP         23  /* Hypercall */
#define EXCCODE_GCM         24  /* Guest CSR modified */
    #define EXCSUBCODE_GCSC 0   /* Software caused */
    #define EXCSUBCODE_GCHC 1   /* Hardware caused */
#define EXCCODE_SE          25  /* Security */

#define EXCCODE_INT_SWI0    (64 + 0)
#define EXCCODE_INT_SWI1    (64 + 1)
#define EXCCODE_INT_HWI0    (64 + 2)
#define EXCCODE_INT_HWI1    (64 + 3)
#define EXCCODE_INT_HWI2    (64 + 4)
#define EXCCODE_INT_HWI3    (64 + 5)
#define EXCCODE_INT_HWI4    (64 + 6)
#define EXCCODE_INT_HWI5    (64 + 7)
#define EXCCODE_INT_HWI6    (64 + 8)
#define EXCCODE_INT_HWI7    (64 + 9)
#define EXCCODE_INT_PMI     (64 + 10)
#define EXCCODE_INT_TI      (64 + 11)
#define EXCCODE_INT_IPI     (64 + 12)

#endif
