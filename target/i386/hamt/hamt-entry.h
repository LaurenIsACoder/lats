/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_ENTRY_H_
#define _HAMT_ENTRY_H_

/*
 * LEAF - declare leaf routine
 */
#define ENTRY(symbol)                                                          \
    .globl symbol;                                                         \
    .align 2;                                                              \
    .type symbol, @function;                                               \
    symbol:

/*
 * END - mark end of function
 */
#define END(function)                   \
        .size   function, .-function

#define INVALID_CODEFLOW_1 .word(0x002b8000 | (0x1))
#define INVALID_CODEFLOW_2 .word(0x002b8000 | (0x2))
#define INVALID_CODEFLOW_3 .word(0x002b8000 | (0x3))
#define INVALID_EBASE_POSITION 0x13

#define HYPERCALL        .word(0x002b8000)
#define ERRORINE         .word(0x00000000)

#define VCPU_FCSR0 0
#define VCPU_VCSR 4
#define VCPU_FCC 8
#define VCPU_FPR0 16
#define VCPU_FPR_LEN 32

#define fcsr0 $r0
#define fcsr1 $r1
#define fcsr2 $r2
#define fcsr3 $r3
#define vcsr16 $r16

#define zero $r0 /* wired zero */
#define ra $r1 /* return address */
#define tp $r2
#define sp $r3 /* stack pointer */
#define v0 $r4 /* return value - caller saved */
#define v1 $r5
#define a0 $r4 /* argument registers */
#define a1 $r5
#define a2 $r6
#define a3 $r7
#define a4 $r8
#define a5 $r9
#define a6 $r10
#define a7 $r11
#define t0 $r12 /* caller saved */
#define t1 $r13
#define t2 $r14
#define t3 $r15
#define t4 $r16
#define t5 $r17
#define t6 $r18
#define t7 $r19
#define t8 $r20
/* $r21: Temporarily reserved */
#define fp $r22 /* frame pointer */
#define s0 $r23 /* callee saved */
#define s1 $r24
#define s2 $r25
#define s3 $r26
#define s4 $r27
#define s5 $r28
#define s6 $r29
#define s7 $r30
#define s8 $r31

#endif
