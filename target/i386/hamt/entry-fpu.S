/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "hamt-entry.h"

ENTRY (get_fpu_regs_lasx)
//    movfcsr2gr    t0, fcsr0
    .word 0x0114C800 | 12
    st.w t0,    a0, VCPU_FCSR0

    movcf2gr    t0, $fcc0
    or    t1, t0, zero
    movcf2gr    t0, $fcc1
    bstrins.d    t1, t0, 0xf, 0x8
    movcf2gr    t0, $fcc2
    bstrins.d    t1, t0, 0x17, 0x10
    movcf2gr    t0, $fcc3
    bstrins.d    t1, t0, 0x1f, 0x18
    movcf2gr    t0, $fcc4
    bstrins.d    t1, t0, 0x27, 0x20
    movcf2gr    t0, $fcc5
    bstrins.d    t1, t0, 0x2f, 0x28
    movcf2gr    t0, $fcc6
    bstrins.d    t1, t0, 0x37, 0x30
    movcf2gr    t0, $fcc7
    bstrins.d    t1, t0, 0x3f, 0x38
    st.d    t1, a0, VCPU_FCC
//    movgr2fcsr    fcsr0, zero
    .word 0x0114C000

  __tmp = 12 /* #define t0 $r12 */
    li.d            t0, VCPU_FPR0
    add.d t0,       a0, t0
    .word (0xb3 << 22 | ((0 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 0)
    .word (0xb3 << 22 | ((1 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 1)
    .word (0xb3 << 22 | ((2 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 2)
    .word (0xb3 << 22 | ((3 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 3)
    .word (0xb3 << 22 | ((4 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 4)
    .word (0xb3 << 22 | ((5 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 5)
    .word (0xb3 << 22 | ((6 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 6)
    .word (0xb3 << 22 | ((7 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 7)
    .word (0xb3 << 22 | ((8 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 8)
    .word (0xb3 << 22 | ((9 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 9)
    .word (0xb3 << 22 | ((10* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 10)
    .word (0xb3 << 22 | ((11* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 11)
    .word (0xb3 << 22 | ((12* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 12)
    .word (0xb3 << 22 | ((13* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 13)
    .word (0xb3 << 22 | ((14* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 14)
    .word (0xb3 << 22 | ((15* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 15)
    .word (0xb3 << 22 | ((16* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 16)
    .word (0xb3 << 22 | ((17* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 17)
    .word (0xb3 << 22 | ((18* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 18)
    .word (0xb3 << 22 | ((19* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 19)
    .word (0xb3 << 22 | ((20* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 20)
    .word (0xb3 << 22 | ((21* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 21)
    .word (0xb3 << 22 | ((22* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 22)
    .word (0xb3 << 22 | ((23* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 23)
    .word (0xb3 << 22 | ((24* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 24)
    .word (0xb3 << 22 | ((25* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 25)
    .word (0xb3 << 22 | ((26* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 26)
    .word (0xb3 << 22 | ((27* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 27)
    .word (0xb3 << 22 | ((28* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 28)
    .word (0xb3 << 22 | ((29* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 29)
    .word (0xb3 << 22 | ((30* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 30)
    .word (0xb3 << 22 | ((31* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 31)

  jirl zero, ra, 0
END (get_fpu_regs_lasx)

ENTRY (get_fpu_regs_lsx)
//    movfcsr2gr    t0, fcsr0
    .word 0x0114C800 | 12
    st.w t0,    a0, VCPU_FCSR0

    movcf2gr    t0, $fcc0
    or    t1, t0, zero
    movcf2gr    t0, $fcc1
    bstrins.d    t1, t0, 0xf, 0x8
    movcf2gr    t0, $fcc2
    bstrins.d    t1, t0, 0x17, 0x10
    movcf2gr    t0, $fcc3
    bstrins.d    t1, t0, 0x1f, 0x18
    movcf2gr    t0, $fcc4
    bstrins.d    t1, t0, 0x27, 0x20
    movcf2gr    t0, $fcc5
    bstrins.d    t1, t0, 0x2f, 0x28
    movcf2gr    t0, $fcc6
    bstrins.d    t1, t0, 0x37, 0x30
    movcf2gr    t0, $fcc7
    bstrins.d    t1, t0, 0x3f, 0x38
    st.d    t1, a0, VCPU_FCC
//    movgr2fcsr    fcsr0, zero
    .word 0x0114C000

  __tmp = 12 /* #define t0 $r12 */
    li.d            t0, VCPU_FPR0
    add.d t0,       a0, t0
    .word (0xb1 << 22 | ((0 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 0)
    .word (0xb1 << 22 | ((1 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 1)
    .word (0xb1 << 22 | ((2 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 2)
    .word (0xb1 << 22 | ((3 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 3)
    .word (0xb1 << 22 | ((4 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 4)
    .word (0xb1 << 22 | ((5 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 5)
    .word (0xb1 << 22 | ((6 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 6)
    .word (0xb1 << 22 | ((7 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 7)
    .word (0xb1 << 22 | ((8 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 8)
    .word (0xb1 << 22 | ((9 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 9)
    .word (0xb1 << 22 | ((10* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 10)
    .word (0xb1 << 22 | ((11* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 11)
    .word (0xb1 << 22 | ((12* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 12)
    .word (0xb1 << 22 | ((13* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 13)
    .word (0xb1 << 22 | ((14* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 14)
    .word (0xb1 << 22 | ((15* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 15)
    .word (0xb1 << 22 | ((16* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 16)
    .word (0xb1 << 22 | ((17* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 17)
    .word (0xb1 << 22 | ((18* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 18)
    .word (0xb1 << 22 | ((19* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 19)
    .word (0xb1 << 22 | ((20* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 20)
    .word (0xb1 << 22 | ((21* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 21)
    .word (0xb1 << 22 | ((22* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 22)
    .word (0xb1 << 22 | ((23* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 23)
    .word (0xb1 << 22 | ((24* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 24)
    .word (0xb1 << 22 | ((25* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 25)
    .word (0xb1 << 22 | ((26* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 26)
    .word (0xb1 << 22 | ((27* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 27)
    .word (0xb1 << 22 | ((28* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 28)
    .word (0xb1 << 22 | ((29* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 29)
    .word (0xb1 << 22 | ((30* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 30)
    .word (0xb1 << 22 | ((31* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 31)

  jirl zero, ra, 0
END (get_fpu_regs_lsx)

ENTRY (get_fpu_regs_fp)
//    movfcsr2gr    t0, fcsr0
    .word 0x0114C800 | 12
    st.w t0,    a0, VCPU_FCSR0

    movcf2gr    t0, $fcc0
    or    t1, t0, zero
    movcf2gr    t0, $fcc1
    bstrins.d    t1, t0, 0xf, 0x8
    movcf2gr    t0, $fcc2
    bstrins.d    t1, t0, 0x17, 0x10
    movcf2gr    t0, $fcc3
    bstrins.d    t1, t0, 0x1f, 0x18
    movcf2gr    t0, $fcc4
    bstrins.d    t1, t0, 0x27, 0x20
    movcf2gr    t0, $fcc5
    bstrins.d    t1, t0, 0x2f, 0x28
    movcf2gr    t0, $fcc6
    bstrins.d    t1, t0, 0x37, 0x30
    movcf2gr    t0, $fcc7
    bstrins.d    t1, t0, 0x3f, 0x38
    st.d    t1, a0, VCPU_FCC
//    movgr2fcsr    fcsr0, zero
    .word 0x0114C000

  __tmp = 12 /* #define t0 $r12 */
    li.d            t0, VCPU_FPR0
    add.d t0,       a0, t0
    .word (0xaf << 22 | ((0 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 0)
    .word (0xaf << 22 | ((1 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 1)
    .word (0xaf << 22 | ((2 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 2)
    .word (0xaf << 22 | ((3 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 3)
    .word (0xaf << 22 | ((4 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 4)
    .word (0xaf << 22 | ((5 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 5)
    .word (0xaf << 22 | ((6 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 6)
    .word (0xaf << 22 | ((7 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 7)
    .word (0xaf << 22 | ((8 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 8)
    .word (0xaf << 22 | ((9 * VCPU_FPR_LEN) << 10) | __tmp  << 5 | 9)
    .word (0xaf << 22 | ((10* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 10)
    .word (0xaf << 22 | ((11* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 11)
    .word (0xaf << 22 | ((12* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 12)
    .word (0xaf << 22 | ((13* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 13)
    .word (0xaf << 22 | ((14* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 14)
    .word (0xaf << 22 | ((15* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 15)
    .word (0xaf << 22 | ((16* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 16)
    .word (0xaf << 22 | ((17* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 17)
    .word (0xaf << 22 | ((18* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 18)
    .word (0xaf << 22 | ((19* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 19)
    .word (0xaf << 22 | ((20* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 20)
    .word (0xaf << 22 | ((21* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 21)
    .word (0xaf << 22 | ((22* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 22)
    .word (0xaf << 22 | ((23* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 23)
    .word (0xaf << 22 | ((24* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 24)
    .word (0xaf << 22 | ((25* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 25)
    .word (0xaf << 22 | ((26* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 26)
    .word (0xaf << 22 | ((27* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 27)
    .word (0xaf << 22 | ((28* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 28)
    .word (0xaf << 22 | ((29* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 29)
    .word (0xaf << 22 | ((30* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 30)
    .word (0xaf << 22 | ((31* VCPU_FPR_LEN) << 10) | __tmp  << 5 | 31)

  jirl zero, ra, 0
END (get_fpu_regs_fp)
