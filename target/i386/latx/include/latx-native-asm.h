/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_NATIVE_ASM_H_
#define _LATX_NATIVE_ASM_H_

#include "latx-asm.h"

#ifdef __loongarch__

static inline void __fld32_native_00(uint64_t *src) { __asm__ __volatile__ ("fld.s $f0,  %0\n\r"::"m"(*src):); }
static inline void __fld32_native_01(uint64_t *src) { __asm__ __volatile__ ("fld.s $f1,  %0\n\r"::"m"(*src):); }
static inline void __fld32_native_02(uint64_t *src) { __asm__ __volatile__ ("fld.s $f2,  %0\n\r"::"m"(*src):); }
static inline void __fld32_native_03(uint64_t *src) { __asm__ __volatile__ ("fld.s $f3,  %0\n\r"::"m"(*src):); }
static inline void __fld32_native_04(uint64_t *src) { __asm__ __volatile__ ("fld.s $f4,  %0\n\r"::"m"(*src):); }
static inline void __fld32_native_05(uint64_t *src) { __asm__ __volatile__ ("fld.s $f5,  %0\n\r"::"m"(*src):); }
static inline void __fld32_native_06(uint64_t *src) { __asm__ __volatile__ ("fld.s $f6,  %0\n\r"::"m"(*src):); }
static inline void __fld32_native_07(uint64_t *src) { __asm__ __volatile__ ("fld.s $f7,  %0\n\r"::"m"(*src):); }
static inline void __fld32_native_08(uint64_t *src) { __asm__ __volatile__ ("fld.s $f8,  %0\n\r"::"m"(*src):); }
static inline void __fld32_native_09(uint64_t *src) { __asm__ __volatile__ ("fld.s $f9,  %0\n\r"::"m"(*src):); }
static inline void __fld32_native_10(uint64_t *src) { __asm__ __volatile__ ("fld.s $f10, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_11(uint64_t *src) { __asm__ __volatile__ ("fld.s $f11, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_12(uint64_t *src) { __asm__ __volatile__ ("fld.s $f12, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_13(uint64_t *src) { __asm__ __volatile__ ("fld.s $f13, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_14(uint64_t *src) { __asm__ __volatile__ ("fld.s $f14, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_15(uint64_t *src) { __asm__ __volatile__ ("fld.s $f15, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_16(uint64_t *src) { __asm__ __volatile__ ("fld.s $f16, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_17(uint64_t *src) { __asm__ __volatile__ ("fld.s $f17, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_18(uint64_t *src) { __asm__ __volatile__ ("fld.s $f18, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_19(uint64_t *src) { __asm__ __volatile__ ("fld.s $f19, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_20(uint64_t *src) { __asm__ __volatile__ ("fld.s $f20, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_21(uint64_t *src) { __asm__ __volatile__ ("fld.s $f21, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_22(uint64_t *src) { __asm__ __volatile__ ("fld.s $f22, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_23(uint64_t *src) { __asm__ __volatile__ ("fld.s $f23, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_24(uint64_t *src) { __asm__ __volatile__ ("fld.s $f24, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_25(uint64_t *src) { __asm__ __volatile__ ("fld.s $f25, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_26(uint64_t *src) { __asm__ __volatile__ ("fld.s $f26, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_27(uint64_t *src) { __asm__ __volatile__ ("fld.s $f27, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_28(uint64_t *src) { __asm__ __volatile__ ("fld.s $f28, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_29(uint64_t *src) { __asm__ __volatile__ ("fld.s $f29, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_30(uint64_t *src) { __asm__ __volatile__ ("fld.s $f30, %0\n\r"::"m"(*src):); }
static inline void __fld32_native_31(uint64_t *src) { __asm__ __volatile__ ("fld.s $f31, %0\n\r"::"m"(*src):); }

static inline void __fst32_native_00(uint64_t *src) { __asm__ __volatile__ ("fst.s $f0,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_01(uint64_t *src) { __asm__ __volatile__ ("fst.s $f1,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_02(uint64_t *src) { __asm__ __volatile__ ("fst.s $f2,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_03(uint64_t *src) { __asm__ __volatile__ ("fst.s $f3,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_04(uint64_t *src) { __asm__ __volatile__ ("fst.s $f4,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_05(uint64_t *src) { __asm__ __volatile__ ("fst.s $f5,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_06(uint64_t *src) { __asm__ __volatile__ ("fst.s $f6,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_07(uint64_t *src) { __asm__ __volatile__ ("fst.s $f7,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_08(uint64_t *src) { __asm__ __volatile__ ("fst.s $f8,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_09(uint64_t *src) { __asm__ __volatile__ ("fst.s $f9,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_10(uint64_t *src) { __asm__ __volatile__ ("fst.s $f10, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_11(uint64_t *src) { __asm__ __volatile__ ("fst.s $f11, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_12(uint64_t *src) { __asm__ __volatile__ ("fst.s $f12, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_13(uint64_t *src) { __asm__ __volatile__ ("fst.s $f13, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_14(uint64_t *src) { __asm__ __volatile__ ("fst.s $f14, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_15(uint64_t *src) { __asm__ __volatile__ ("fst.s $f15, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_16(uint64_t *src) { __asm__ __volatile__ ("fst.s $f16, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_17(uint64_t *src) { __asm__ __volatile__ ("fst.s $f17, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_18(uint64_t *src) { __asm__ __volatile__ ("fst.s $f18, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_19(uint64_t *src) { __asm__ __volatile__ ("fst.s $f19, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_20(uint64_t *src) { __asm__ __volatile__ ("fst.s $f20, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_21(uint64_t *src) { __asm__ __volatile__ ("fst.s $f21, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_22(uint64_t *src) { __asm__ __volatile__ ("fst.s $f22, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_23(uint64_t *src) { __asm__ __volatile__ ("fst.s $f23, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_24(uint64_t *src) { __asm__ __volatile__ ("fst.s $f24, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_25(uint64_t *src) { __asm__ __volatile__ ("fst.s $f25, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_26(uint64_t *src) { __asm__ __volatile__ ("fst.s $f26, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_27(uint64_t *src) { __asm__ __volatile__ ("fst.s $f27, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_28(uint64_t *src) { __asm__ __volatile__ ("fst.s $f28, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_29(uint64_t *src) { __asm__ __volatile__ ("fst.s $f29, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_30(uint64_t *src) { __asm__ __volatile__ ("fst.s $f30, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst32_native_31(uint64_t *src) { __asm__ __volatile__ ("fst.s $f31, %0\n\r"::"m"(*src):"memory"); }

static inline void __fld64_native_00(uint64_t *src) { __asm__ __volatile__ ("fld.d $f0,  %0\n\r"::"m"(*src):); }
static inline void __fld64_native_01(uint64_t *src) { __asm__ __volatile__ ("fld.d $f1,  %0\n\r"::"m"(*src):); }
static inline void __fld64_native_02(uint64_t *src) { __asm__ __volatile__ ("fld.d $f2,  %0\n\r"::"m"(*src):); }
static inline void __fld64_native_03(uint64_t *src) { __asm__ __volatile__ ("fld.d $f3,  %0\n\r"::"m"(*src):); }
static inline void __fld64_native_04(uint64_t *src) { __asm__ __volatile__ ("fld.d $f4,  %0\n\r"::"m"(*src):); }
static inline void __fld64_native_05(uint64_t *src) { __asm__ __volatile__ ("fld.d $f5,  %0\n\r"::"m"(*src):); }
static inline void __fld64_native_06(uint64_t *src) { __asm__ __volatile__ ("fld.d $f6,  %0\n\r"::"m"(*src):); }
static inline void __fld64_native_07(uint64_t *src) { __asm__ __volatile__ ("fld.d $f7,  %0\n\r"::"m"(*src):); }
static inline void __fld64_native_08(uint64_t *src) { __asm__ __volatile__ ("fld.d $f8,  %0\n\r"::"m"(*src):); }
static inline void __fld64_native_09(uint64_t *src) { __asm__ __volatile__ ("fld.d $f9,  %0\n\r"::"m"(*src):); }
static inline void __fld64_native_10(uint64_t *src) { __asm__ __volatile__ ("fld.d $f10, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_11(uint64_t *src) { __asm__ __volatile__ ("fld.d $f11, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_12(uint64_t *src) { __asm__ __volatile__ ("fld.d $f12, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_13(uint64_t *src) { __asm__ __volatile__ ("fld.d $f13, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_14(uint64_t *src) { __asm__ __volatile__ ("fld.d $f14, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_15(uint64_t *src) { __asm__ __volatile__ ("fld.d $f15, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_16(uint64_t *src) { __asm__ __volatile__ ("fld.d $f16, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_17(uint64_t *src) { __asm__ __volatile__ ("fld.d $f17, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_18(uint64_t *src) { __asm__ __volatile__ ("fld.d $f18, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_19(uint64_t *src) { __asm__ __volatile__ ("fld.d $f19, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_20(uint64_t *src) { __asm__ __volatile__ ("fld.d $f20, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_21(uint64_t *src) { __asm__ __volatile__ ("fld.d $f21, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_22(uint64_t *src) { __asm__ __volatile__ ("fld.d $f22, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_23(uint64_t *src) { __asm__ __volatile__ ("fld.d $f23, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_24(uint64_t *src) { __asm__ __volatile__ ("fld.d $f24, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_25(uint64_t *src) { __asm__ __volatile__ ("fld.d $f25, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_26(uint64_t *src) { __asm__ __volatile__ ("fld.d $f26, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_27(uint64_t *src) { __asm__ __volatile__ ("fld.d $f27, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_28(uint64_t *src) { __asm__ __volatile__ ("fld.d $f28, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_29(uint64_t *src) { __asm__ __volatile__ ("fld.d $f29, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_30(uint64_t *src) { __asm__ __volatile__ ("fld.d $f30, %0\n\r"::"m"(*src):); }
static inline void __fld64_native_31(uint64_t *src) { __asm__ __volatile__ ("fld.d $f31, %0\n\r"::"m"(*src):); }

static inline void __fst64_native_00(uint64_t *src) { __asm__ __volatile__ ("fst.d $f0,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_01(uint64_t *src) { __asm__ __volatile__ ("fst.d $f1,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_02(uint64_t *src) { __asm__ __volatile__ ("fst.d $f2,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_03(uint64_t *src) { __asm__ __volatile__ ("fst.d $f3,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_04(uint64_t *src) { __asm__ __volatile__ ("fst.d $f4,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_05(uint64_t *src) { __asm__ __volatile__ ("fst.d $f5,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_06(uint64_t *src) { __asm__ __volatile__ ("fst.d $f6,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_07(uint64_t *src) { __asm__ __volatile__ ("fst.d $f7,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_08(uint64_t *src) { __asm__ __volatile__ ("fst.d $f8,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_09(uint64_t *src) { __asm__ __volatile__ ("fst.d $f9,  %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_10(uint64_t *src) { __asm__ __volatile__ ("fst.d $f10, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_11(uint64_t *src) { __asm__ __volatile__ ("fst.d $f11, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_12(uint64_t *src) { __asm__ __volatile__ ("fst.d $f12, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_13(uint64_t *src) { __asm__ __volatile__ ("fst.d $f13, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_14(uint64_t *src) { __asm__ __volatile__ ("fst.d $f14, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_15(uint64_t *src) { __asm__ __volatile__ ("fst.d $f15, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_16(uint64_t *src) { __asm__ __volatile__ ("fst.d $f16, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_17(uint64_t *src) { __asm__ __volatile__ ("fst.d $f17, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_18(uint64_t *src) { __asm__ __volatile__ ("fst.d $f18, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_19(uint64_t *src) { __asm__ __volatile__ ("fst.d $f19, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_20(uint64_t *src) { __asm__ __volatile__ ("fst.d $f20, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_21(uint64_t *src) { __asm__ __volatile__ ("fst.d $f21, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_22(uint64_t *src) { __asm__ __volatile__ ("fst.d $f22, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_23(uint64_t *src) { __asm__ __volatile__ ("fst.d $f23, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_24(uint64_t *src) { __asm__ __volatile__ ("fst.d $f24, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_25(uint64_t *src) { __asm__ __volatile__ ("fst.d $f25, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_26(uint64_t *src) { __asm__ __volatile__ ("fst.d $f26, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_27(uint64_t *src) { __asm__ __volatile__ ("fst.d $f27, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_28(uint64_t *src) { __asm__ __volatile__ ("fst.d $f28, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_29(uint64_t *src) { __asm__ __volatile__ ("fst.d $f29, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_30(uint64_t *src) { __asm__ __volatile__ ("fst.d $f30, %0\n\r"::"m"(*src):"memory"); }
static inline void __fst64_native_31(uint64_t *src) { __asm__ __volatile__ ("fst.d $f31, %0\n\r"::"m"(*src):"memory"); }

static inline void __fld32_native_freg_temp(uint64_t *src, int freg)
{
    switch (freg) {
    case 9:  __fld32_native_09(src); break;
    case 10: __fld32_native_10(src); break;
    case 11: __fld32_native_11(src); break;
    case 12: __fld32_native_12(src); break;
    case 13: __fld32_native_13(src); break;
    case 14: __fld32_native_14(src); break;
    case 15: __fld32_native_15(src); break;
    default: break;
    }
}

static inline void __fst32_native_freg_temp(uint64_t *src, int freg)
{
    switch (freg) {
    case 9:  __fst32_native_09(src); break;
    case 10: __fst32_native_10(src); break;
    case 11: __fst32_native_11(src); break;
    case 12: __fst32_native_12(src); break;
    case 13: __fst32_native_13(src); break;
    case 14: __fst32_native_14(src); break;
    case 15: __fst32_native_15(src); break;
    default: break;
    }
}

static inline void __fld64_native_freg_temp(uint64_t *src, int freg)
{
    switch (freg) {
    case 8:  __fld64_native_08(src); break;
    case 9:  __fld64_native_09(src); break;
    case 10: __fld64_native_10(src); break;
    case 11: __fld64_native_11(src); break;
    case 12: __fld64_native_12(src); break;
    case 13: __fld64_native_13(src); break;
    case 14: __fld64_native_14(src); break;
    case 15: __fld64_native_15(src); break;
    default: break;
    }
}

static inline void __fst64_native_freg_temp(uint64_t *src, int freg)
{
    switch (freg) {
    case 8:  __fst64_native_08(src); break;
    case 9:  __fst64_native_09(src); break;
    case 10: __fst64_native_10(src); break;
    case 11: __fst64_native_11(src); break;
    case 12: __fst64_native_12(src); break;
    case 13: __fst64_native_13(src); break;
    case 14: __fst64_native_14(src); break;
    case 15: __fst64_native_15(src); break;
    default: break;
    }
}

static inline void __movgr2fr32_native_00(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f0,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_01(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f1,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_02(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f2,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_03(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f3,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_04(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f4,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_05(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f5,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_06(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f6,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_07(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f7,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_08(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f8,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_09(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f9,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_10(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f10, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_11(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f11, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_12(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f12, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_13(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f13, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_14(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f14, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_15(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f15, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_16(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f16, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_17(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f17, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_18(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f18, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_19(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f19, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_20(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f20, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_21(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f21, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_22(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f22, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_23(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f23, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_24(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f24, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_25(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f25, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_26(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f26, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_27(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f27, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_28(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f28, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_29(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f29, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_30(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f30, %0\n\r"::"r"(src):); }
static inline void __movgr2fr32_native_31(uint64_t src) { __asm__ __volatile__ ("movgr2fr.w $f31, %0\n\r"::"r"(src):); }

static inline int32_t __movfr2gr32_native_00(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f0 \n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_01(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f1 \n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_02(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f2 \n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_03(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f3 \n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_04(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f4 \n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_05(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f5 \n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_06(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f6 \n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_07(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f7 \n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_08(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f8 \n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_09(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f9 \n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_10(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f10\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_11(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f11\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_12(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f12\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_13(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f13\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_14(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f14\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_15(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f15\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_16(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f16\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_17(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f17\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_18(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f18\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_19(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f19\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_20(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f20\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_21(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f21\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_22(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f22\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_23(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f23\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_24(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f24\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_25(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f25\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_26(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f26\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_27(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f27\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_28(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f28\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_29(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f29\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_30(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f30\n\r":"=r"(val)::); return val; }
static inline int32_t __movfr2gr32_native_31(void) { int32_t val; __asm__ __volatile__ ("movfr2gr.s %0, $f31\n\r":"=r"(val)::); return val; }

static inline void __movgr2fr64_native_00(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f0,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_01(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f1,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_02(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f2,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_03(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f3,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_04(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f4,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_05(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f5,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_06(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f6,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_07(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f7,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_08(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f8,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_09(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f9,  %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_10(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f10, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_11(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f11, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_12(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f12, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_13(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f13, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_14(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f14, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_15(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f15, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_16(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f16, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_17(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f17, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_18(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f18, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_19(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f19, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_20(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f20, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_21(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f21, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_22(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f22, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_23(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f23, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_24(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f24, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_25(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f25, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_26(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f26, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_27(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f27, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_28(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f28, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_29(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f29, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_30(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f30, %0\n\r"::"r"(src):); }
static inline void __movgr2fr64_native_31(uint64_t src) { __asm__ __volatile__ ("movgr2fr.d $f31, %0\n\r"::"r"(src):); }

static inline uint64_t __movfr2gr64_native_00(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f0 \n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_01(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f1 \n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_02(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f2 \n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_03(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f3 \n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_04(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f4 \n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_05(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f5 \n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_06(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f6 \n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_07(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f7 \n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_08(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f8 \n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_09(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f9 \n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_10(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f10\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_11(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f11\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_12(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f12\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_13(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f13\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_14(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f14\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_15(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f15\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_16(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f16\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_17(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f17\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_18(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f18\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_19(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f19\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_20(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f20\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_21(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f21\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_22(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f22\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_23(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f23\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_24(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f24\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_25(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f25\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_26(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f26\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_27(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f27\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_28(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f28\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_29(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f29\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_30(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f30\n\r":"=r"(val)::); return val; }
static inline uint64_t __movfr2gr64_native_31(void) { uint64_t val; __asm__ __volatile__ ("movfr2gr.d %0, $f31\n\r":"=r"(val)::); return val; }

static inline void __movgr2fr32_native_freg_temp(uint64_t src, int freg)
{
    switch (freg) {
    case 0:  __movgr2fr32_native_00(src); break;
    case 1: __movgr2fr32_native_01(src); break;
    case 2: __movgr2fr32_native_02(src); break;
    case 3: __movgr2fr32_native_03(src); break;
    case 4: __movgr2fr32_native_04(src); break;
    case 5: __movgr2fr32_native_05(src); break;
    case 6: __movgr2fr32_native_06(src); break;
    case 7: __movgr2fr32_native_07(src); break;
    case 8: __movgr2fr32_native_08(src); break;
    case 9:  __movgr2fr32_native_09(src); break;
    case 10: __movgr2fr32_native_10(src); break;
    case 11: __movgr2fr32_native_11(src); break;
    case 12: __movgr2fr32_native_12(src); break;
    case 13: __movgr2fr32_native_13(src); break;
    case 14: __movgr2fr32_native_14(src); break;
    case 15: __movgr2fr32_native_15(src); break;
    case 16: __movgr2fr32_native_16(src); break;
    case 17: __movgr2fr32_native_17(src); break;
    case 18: __movgr2fr32_native_18(src); break;
    case 19: __movgr2fr32_native_19(src); break;
    case 20: __movgr2fr32_native_20(src); break;
    case 21: __movgr2fr32_native_21(src); break;
    case 22: __movgr2fr32_native_22(src); break;
    case 23: __movgr2fr32_native_23(src); break;
    case 24: __movgr2fr32_native_24(src); break;
    case 25: __movgr2fr32_native_25(src); break;
    case 26: __movgr2fr32_native_26(src); break;
    case 27: __movgr2fr32_native_27(src); break;
    case 28: __movgr2fr32_native_28(src); break;
    case 29: __movgr2fr32_native_29(src); break;
    case 30: __movgr2fr32_native_30(src); break;
    case 31: __movgr2fr32_native_31(src); break;
    default: break;
    }
}

static inline int32_t __movfr2gr32_native_freg_temp(int freg)
{
    int32_t val = 0;
    switch (freg) {
    case 0: val = __movfr2gr32_native_00(); break;
    case 1: val = __movfr2gr32_native_01(); break;
    case 2: val = __movfr2gr32_native_02(); break;
    case 3: val = __movfr2gr32_native_03(); break;
    case 4: val = __movfr2gr32_native_04(); break;
    case 5: val = __movfr2gr32_native_05(); break;
    case 6: val = __movfr2gr32_native_06(); break;
    case 7: val = __movfr2gr32_native_07(); break;
    case 8: val = __movfr2gr32_native_08(); break;
    case 9:  val = __movfr2gr32_native_09(); break;
    case 10: val = __movfr2gr32_native_10(); break;
    case 11: val = __movfr2gr32_native_11(); break;
    case 12: val = __movfr2gr32_native_12(); break;
    case 13: val = __movfr2gr32_native_13(); break;
    case 14: val = __movfr2gr32_native_14(); break;
    case 15: val = __movfr2gr32_native_15(); break;
    case 16: val = __movfr2gr32_native_16(); break;
    case 17: val = __movfr2gr32_native_17(); break;
    case 18: val = __movfr2gr32_native_18(); break;
    case 19: val = __movfr2gr32_native_19(); break;
    case 20: val = __movfr2gr32_native_20(); break;
    case 21: val = __movfr2gr32_native_21(); break;
    case 22: val = __movfr2gr32_native_22(); break;
    case 23: val = __movfr2gr32_native_23(); break;
    case 24: val = __movfr2gr32_native_24(); break;
    case 25: val = __movfr2gr32_native_25(); break;
    case 26: val = __movfr2gr32_native_26(); break;
    case 27: val = __movfr2gr32_native_27(); break;
    case 28: val = __movfr2gr32_native_28(); break;
    case 29: val = __movfr2gr32_native_29(); break;
    case 30: val = __movfr2gr32_native_30(); break;
    case 31: val = __movfr2gr32_native_31(); break;
    default: break;
    }
    return val;
}

static inline void __movgr2fr64_native_freg_temp(uint64_t src, int freg)
{
    switch (freg) {
    case 0:  __movgr2fr64_native_00(src); break;
    case 1: __movgr2fr64_native_01(src); break;
    case 2: __movgr2fr64_native_02(src); break;
    case 3: __movgr2fr64_native_03(src); break;
    case 4: __movgr2fr64_native_04(src); break;
    case 5: __movgr2fr64_native_05(src); break;
    case 6: __movgr2fr64_native_06(src); break;
    case 7: __movgr2fr64_native_07(src); break;
    case 8: __movgr2fr64_native_08(src); break;
    case 9:  __movgr2fr64_native_09(src); break;
    case 10: __movgr2fr64_native_10(src); break;
    case 11: __movgr2fr64_native_11(src); break;
    case 12: __movgr2fr64_native_12(src); break;
    case 13: __movgr2fr64_native_13(src); break;
    case 14: __movgr2fr64_native_14(src); break;
    case 15: __movgr2fr64_native_15(src); break;
    case 16: __movgr2fr64_native_16(src); break;
    case 17: __movgr2fr64_native_17(src); break;
    case 18: __movgr2fr64_native_18(src); break;
    case 19: __movgr2fr64_native_19(src); break;
    case 20: __movgr2fr64_native_20(src); break;
    case 21: __movgr2fr64_native_21(src); break;
    case 22: __movgr2fr64_native_22(src); break;
    case 23: __movgr2fr64_native_23(src); break;
    case 24: __movgr2fr64_native_24(src); break;
    case 25: __movgr2fr64_native_25(src); break;
    case 26: __movgr2fr64_native_26(src); break;
    case 27: __movgr2fr64_native_27(src); break;
    case 28: __movgr2fr64_native_28(src); break;
    case 29: __movgr2fr64_native_29(src); break;
    case 30: __movgr2fr64_native_30(src); break;
    case 31: __movgr2fr64_native_31(src); break;
    default: break;
    }
}

static inline uint64_t __movfr2gr64_native_freg_temp(int freg)
{
    uint64_t val = 0;
    switch (freg) {
    case 0:  val = __movfr2gr64_native_00(); break;
    case 1:  val = __movfr2gr64_native_01(); break;
    case 2:  val = __movfr2gr64_native_02(); break;
    case 3:  val = __movfr2gr64_native_03(); break;
    case 4:  val = __movfr2gr64_native_04(); break;
    case 5:  val = __movfr2gr64_native_05(); break;
    case 6:  val = __movfr2gr64_native_06(); break;
    case 7:  val = __movfr2gr64_native_07(); break;
    case 8:  val = __movfr2gr64_native_08(); break;
    case 9:  val = __movfr2gr64_native_09(); break;
    case 10: val = __movfr2gr64_native_10(); break;
    case 11: val = __movfr2gr64_native_11(); break;
    case 12: val = __movfr2gr64_native_12(); break;
    case 13: val = __movfr2gr64_native_13(); break;
    case 14: val = __movfr2gr64_native_14(); break;
    case 15: val = __movfr2gr64_native_15(); break;
    case 16: val = __movfr2gr64_native_16(); break;
    case 17: val = __movfr2gr64_native_17(); break;
    case 18: val = __movfr2gr64_native_18(); break;
    case 19: val = __movfr2gr64_native_19(); break;
    case 20: val = __movfr2gr64_native_20(); break;
    case 21: val = __movfr2gr64_native_21(); break;
    case 22: val = __movfr2gr64_native_22(); break;
    case 23: val = __movfr2gr64_native_23(); break;
    case 24: val = __movfr2gr64_native_24(); break;
    case 25: val = __movfr2gr64_native_25(); break;
    case 26: val = __movfr2gr64_native_26(); break;
    case 27: val = __movfr2gr64_native_27(); break;
    case 28: val = __movfr2gr64_native_28(); break;
    case 29: val = __movfr2gr64_native_29(); break;
    case 30: val = __movfr2gr64_native_30(); break;
    case 31: val = __movfr2gr64_native_31(); break;
    default: break;
    }
    return val;
}

static inline void __vld128_native_00(unsigned char *src) { __asm__ __volatile__ ("vld $vr0 , %0\n\r"::"m"(*src):); }
static inline void __vld128_native_01(unsigned char *src) { __asm__ __volatile__ ("vld $vr1 , %0\n\r"::"m"(*src):); }
static inline void __vld128_native_02(unsigned char *src) { __asm__ __volatile__ ("vld $vr2 , %0\n\r"::"m"(*src):); }
static inline void __vld128_native_03(unsigned char *src) { __asm__ __volatile__ ("vld $vr3 , %0\n\r"::"m"(*src):); }
static inline void __vld128_native_04(unsigned char *src) { __asm__ __volatile__ ("vld $vr4 , %0\n\r"::"m"(*src):); }
static inline void __vld128_native_05(unsigned char *src) { __asm__ __volatile__ ("vld $vr5 , %0\n\r"::"m"(*src):); }
static inline void __vld128_native_06(unsigned char *src) { __asm__ __volatile__ ("vld $vr6 , %0\n\r"::"m"(*src):); }
static inline void __vld128_native_07(unsigned char *src) { __asm__ __volatile__ ("vld $vr7 , %0\n\r"::"m"(*src):); }
static inline void __vld128_native_08(unsigned char *src) { __asm__ __volatile__ ("vld $vr8 , %0\n\r"::"m"(*src):); }
static inline void __vld128_native_09(unsigned char *src) { __asm__ __volatile__ ("vld $vr9 , %0\n\r"::"m"(*src):); }
static inline void __vld128_native_10(unsigned char *src) { __asm__ __volatile__ ("vld $vr10, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_11(unsigned char *src) { __asm__ __volatile__ ("vld $vr11, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_12(unsigned char *src) { __asm__ __volatile__ ("vld $vr12, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_13(unsigned char *src) { __asm__ __volatile__ ("vld $vr13, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_14(unsigned char *src) { __asm__ __volatile__ ("vld $vr14, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_15(unsigned char *src) { __asm__ __volatile__ ("vld $vr15, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_16(unsigned char *src) { __asm__ __volatile__ ("vld $vr16, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_17(unsigned char *src) { __asm__ __volatile__ ("vld $vr17, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_18(unsigned char *src) { __asm__ __volatile__ ("vld $vr18, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_19(unsigned char *src) { __asm__ __volatile__ ("vld $vr19, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_20(unsigned char *src) { __asm__ __volatile__ ("vld $vr20, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_21(unsigned char *src) { __asm__ __volatile__ ("vld $vr21, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_22(unsigned char *src) { __asm__ __volatile__ ("vld $vr22, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_23(unsigned char *src) { __asm__ __volatile__ ("vld $vr23, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_24(unsigned char *src) { __asm__ __volatile__ ("vld $vr24, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_25(unsigned char *src) { __asm__ __volatile__ ("vld $vr25, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_26(unsigned char *src) { __asm__ __volatile__ ("vld $vr26, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_27(unsigned char *src) { __asm__ __volatile__ ("vld $vr27, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_28(unsigned char *src) { __asm__ __volatile__ ("vld $vr28, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_29(unsigned char *src) { __asm__ __volatile__ ("vld $vr29, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_30(unsigned char *src) { __asm__ __volatile__ ("vld $vr30, %0\n\r"::"m"(*src):); }
static inline void __vld128_native_31(unsigned char *src) { __asm__ __volatile__ ("vld $vr31, %0\n\r"::"m"(*src):); }

static inline void __vst128_native_00(unsigned char *res) { __asm__ __volatile__ ("vst $vr0,  %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_01(unsigned char *res) { __asm__ __volatile__ ("vst $vr1,  %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_02(unsigned char *res) { __asm__ __volatile__ ("vst $vr2,  %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_03(unsigned char *res) { __asm__ __volatile__ ("vst $vr3,  %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_04(unsigned char *res) { __asm__ __volatile__ ("vst $vr4,  %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_05(unsigned char *res) { __asm__ __volatile__ ("vst $vr5,  %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_06(unsigned char *res) { __asm__ __volatile__ ("vst $vr6,  %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_07(unsigned char *res) { __asm__ __volatile__ ("vst $vr7,  %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_08(unsigned char *res) { __asm__ __volatile__ ("vst $vr8,  %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_09(unsigned char *res) { __asm__ __volatile__ ("vst $vr9,  %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_10(unsigned char *res) { __asm__ __volatile__ ("vst $vr10, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_11(unsigned char *res) { __asm__ __volatile__ ("vst $vr11, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_12(unsigned char *res) { __asm__ __volatile__ ("vst $vr12, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_13(unsigned char *res) { __asm__ __volatile__ ("vst $vr13, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_14(unsigned char *res) { __asm__ __volatile__ ("vst $vr14, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_15(unsigned char *res) { __asm__ __volatile__ ("vst $vr15, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_16(unsigned char *res) { __asm__ __volatile__ ("vst $vr16, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_17(unsigned char *res) { __asm__ __volatile__ ("vst $vr17, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_18(unsigned char *res) { __asm__ __volatile__ ("vst $vr18, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_19(unsigned char *res) { __asm__ __volatile__ ("vst $vr19, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_20(unsigned char *res) { __asm__ __volatile__ ("vst $vr20, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_21(unsigned char *res) { __asm__ __volatile__ ("vst $vr21, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_22(unsigned char *res) { __asm__ __volatile__ ("vst $vr22, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_23(unsigned char *res) { __asm__ __volatile__ ("vst $vr23, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_24(unsigned char *res) { __asm__ __volatile__ ("vst $vr24, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_25(unsigned char *res) { __asm__ __volatile__ ("vst $vr25, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_26(unsigned char *res) { __asm__ __volatile__ ("vst $vr26, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_27(unsigned char *res) { __asm__ __volatile__ ("vst $vr27, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_28(unsigned char *res) { __asm__ __volatile__ ("vst $vr28, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_29(unsigned char *res) { __asm__ __volatile__ ("vst $vr29, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_30(unsigned char *res) { __asm__ __volatile__ ("vst $vr30, %0\n\r"::"m"(*res):"memory"); }
static inline void __vst128_native_31(unsigned char *res) { __asm__ __volatile__ ("vst $vr31, %0\n\r"::"m"(*res):"memory"); }


static inline void __vld128_native_vreg_temp(void *ptr, int vreg)
{
    switch (vreg) {
    case 9:  __vld128_native_09(ptr); break;
    case 10: __vld128_native_10(ptr); break;
    case 11: __vld128_native_11(ptr); break;
    case 12: __vld128_native_12(ptr); break;
    case 13: __vld128_native_13(ptr); break;
    case 14: __vld128_native_14(ptr); break;
    case 15: __vld128_native_15(ptr); break;

    case 16: __vld128_native_16(ptr); break;
    case 17: __vld128_native_17(ptr); break;
    case 18: __vld128_native_18(ptr); break;
    case 19: __vld128_native_19(ptr); break;
    case 20: __vld128_native_20(ptr); break;
    case 21: __vld128_native_21(ptr); break;
    case 22: __vld128_native_22(ptr); break;
    case 23: __vld128_native_23(ptr); break;
    case 24: __vld128_native_24(ptr); break;
    case 25: __vld128_native_25(ptr); break;
    case 26: __vld128_native_26(ptr); break;
    case 27: __vld128_native_27(ptr); break;
    case 28: __vld128_native_28(ptr); break;
    case 29: __vld128_native_29(ptr); break;
    case 30: __vld128_native_30(ptr); break;
    case 31: __vld128_native_31(ptr); break;
    default: break;
    }
}

static inline void __vst128_native_vreg_temp(void *ptr, int vreg)
{
    switch (vreg) {
    case 9:  __vst128_native_09(ptr); break;
    case 10: __vst128_native_10(ptr); break;
    case 11: __vst128_native_11(ptr); break;
    case 12: __vst128_native_12(ptr); break;
    case 13: __vst128_native_13(ptr); break;
    case 14: __vst128_native_14(ptr); break;
    case 15: __vst128_native_15(ptr); break;
    case 16: __vst128_native_16(ptr); break;
    case 17: __vst128_native_17(ptr); break;
    case 18: __vst128_native_18(ptr); break;
    case 19: __vst128_native_19(ptr); break;
    case 20: __vst128_native_20(ptr); break;
    case 21: __vst128_native_21(ptr); break;
    case 22: __vst128_native_22(ptr); break;
    case 23: __vst128_native_23(ptr); break;
    case 24: __vst128_native_24(ptr); break;
    case 25: __vst128_native_25(ptr); break;
    case 26: __vst128_native_26(ptr); break;
    case 27: __vst128_native_27(ptr); break;
    case 28: __vst128_native_28(ptr); break;
    case 29: __vst128_native_29(ptr); break;
    case 30: __vst128_native_30(ptr); break;
    case 31: __vst128_native_31(ptr); break;
    default: break;
    }
}

#ifdef CONFIG_LATX_OPT_NOFP
static inline void __nofp_sync_fpr_to_env(CPUState * cpu, int reason)
{
    CPUArchState *env = cpu->env_ptr;

    if(env->nofp) {
        __fst64_native_00(&(env->fpregs[0].d.low));
        __fst64_native_01(&(env->fpregs[1].d.low));
        __fst64_native_02(&(env->fpregs[2].d.low));
        __fst64_native_03(&(env->fpregs[3].d.low));
        __fst64_native_04(&(env->fpregs[4].d.low));
        __fst64_native_05(&(env->fpregs[5].d.low));
        __fst64_native_06(&(env->fpregs[6].d.low));
        __fst64_native_07(&(env->fpregs[7].d.low));
        env->nofp=0;
        //printf("exit from mmx %d\n", resion);
    }
}

static inline void __nofp_sync_xmm_to_env(CPUState * cpu)
{
    CPUArchState *env = cpu->env_ptr;
    __vst128_native_16(&(env->xmm_regs[0]._b_ZMMReg[0]));
    __vst128_native_17(&(env->xmm_regs[1]._b_ZMMReg[0]));
    __vst128_native_18(&(env->xmm_regs[2]._b_ZMMReg[0]));
    __vst128_native_19(&(env->xmm_regs[3]._b_ZMMReg[0]));
    __vst128_native_20(&(env->xmm_regs[4]._b_ZMMReg[0]));
    __vst128_native_21(&(env->xmm_regs[5]._b_ZMMReg[0]));
    __vst128_native_22(&(env->xmm_regs[6]._b_ZMMReg[0]));
    __vst128_native_23(&(env->xmm_regs[7]._b_ZMMReg[0]));
    __vst128_native_24(&(env->xmm_regs[8]._b_ZMMReg[0]));
    __vst128_native_25(&(env->xmm_regs[9]._b_ZMMReg[0]));
    __vst128_native_26(&(env->xmm_regs[10]._b_ZMMReg[0]));
    __vst128_native_27(&(env->xmm_regs[11]._b_ZMMReg[0]));
    __vst128_native_28(&(env->xmm_regs[12]._b_ZMMReg[0]));
    __vst128_native_29(&(env->xmm_regs[13]._b_ZMMReg[0]));
    __vst128_native_30(&(env->xmm_regs[14]._b_ZMMReg[0]));
    __vst128_native_31(&(env->xmm_regs[15]._b_ZMMReg[0]));
}

static inline void __nofp_sync_xmm_from_env(CPUState * cpu)
{
    CPUArchState *env = cpu->env_ptr;
    __vld128_native_16(&(env->xmm_regs[0]._b_ZMMReg[0]));
    __vld128_native_17(&(env->xmm_regs[1]._b_ZMMReg[0]));
    __vld128_native_18(&(env->xmm_regs[2]._b_ZMMReg[0]));
    __vld128_native_19(&(env->xmm_regs[3]._b_ZMMReg[0]));
    __vld128_native_20(&(env->xmm_regs[4]._b_ZMMReg[0]));
    __vld128_native_21(&(env->xmm_regs[5]._b_ZMMReg[0]));
    __vld128_native_22(&(env->xmm_regs[6]._b_ZMMReg[0]));
    __vld128_native_23(&(env->xmm_regs[7]._b_ZMMReg[0]));
    __vld128_native_24(&(env->xmm_regs[8]._b_ZMMReg[0]));
    __vld128_native_25(&(env->xmm_regs[9]._b_ZMMReg[0]));
    __vld128_native_26(&(env->xmm_regs[10]._b_ZMMReg[0]));
    __vld128_native_27(&(env->xmm_regs[11]._b_ZMMReg[0]));
    __vld128_native_28(&(env->xmm_regs[12]._b_ZMMReg[0]));
    __vld128_native_29(&(env->xmm_regs[13]._b_ZMMReg[0]));
    __vld128_native_30(&(env->xmm_regs[14]._b_ZMMReg[0]));
    __vld128_native_31(&(env->xmm_regs[15]._b_ZMMReg[0]));
}
#endif

static inline void __write_native_fcc0(int n) { __asm__ __volatile__ ("movgr2cf $fcc0, %0\n\r"::"r"(n):); }
static inline void __write_native_fcc1(int n) { __asm__ __volatile__ ("movgr2cf $fcc1, %0\n\r"::"r"(n):); }
static inline void __write_native_fcc2(int n) { __asm__ __volatile__ ("movgr2cf $fcc2, %0\n\r"::"r"(n):); }
static inline void __write_native_fcc3(int n) { __asm__ __volatile__ ("movgr2cf $fcc3, %0\n\r"::"r"(n):); }
static inline void __write_native_fcc4(int n) { __asm__ __volatile__ ("movgr2cf $fcc4, %0\n\r"::"r"(n):); }
static inline void __write_native_fcc5(int n) { __asm__ __volatile__ ("movgr2cf $fcc5, %0\n\r"::"r"(n):); }
static inline void __write_native_fcc6(int n) { __asm__ __volatile__ ("movgr2cf $fcc6, %0\n\r"::"r"(n):); }
static inline void __write_native_fcc7(int n) { __asm__ __volatile__ ("movgr2cf $fcc7, %0\n\r"::"r"(n):); }

static inline int  __read_native_fcc0(void) { int val; __asm__ __volatile__ ("movcf2gr %0, fcc0\n\r":"=r"(val)::); return val; }
static inline int  __read_native_fcc1(void) { int val; __asm__ __volatile__ ("movcf2gr %0, fcc1\n\r":"=r"(val)::); return val; }
static inline int  __read_native_fcc2(void) { int val; __asm__ __volatile__ ("movcf2gr %0, fcc2\n\r":"=r"(val)::); return val; }
static inline int  __read_native_fcc3(void) { int val; __asm__ __volatile__ ("movcf2gr %0, fcc3\n\r":"=r"(val)::); return val; }
static inline int  __read_native_fcc4(void) { int val; __asm__ __volatile__ ("movcf2gr %0, fcc4\n\r":"=r"(val)::); return val; }
static inline int  __read_native_fcc5(void) { int val; __asm__ __volatile__ ("movcf2gr %0, fcc5\n\r":"=r"(val)::); return val; }
static inline int  __read_native_fcc6(void) { int val; __asm__ __volatile__ ("movcf2gr %0, fcc6\n\r":"=r"(val)::); return val; }
static inline int  __read_native_fcc7(void) { int val; __asm__ __volatile__ ("movcf2gr %0, fcc7\n\r":"=r"(val)::); return val; }

static inline void __write_native_fcsr0(uint32_t fcsr) { __asm__ __volatile__ (MOVGR_2_FCSR0(%0)::"r"(fcsr):); }
static inline void __write_native_fcsr1(uint32_t fcsr) { __asm__ __volatile__ (MOVGR_2_FCSR1(%0)::"r"(fcsr):); }
static inline void __write_native_fcsr2(uint32_t fcsr) { __asm__ __volatile__ (MOVGR_2_FCSR2(%0)::"r"(fcsr):); }
static inline void __write_native_fcsr3(uint32_t fcsr) { __asm__ __volatile__ (MOVGR_2_FCSR3(%0)::"r"(fcsr):); }

static inline uint32_t __read_native_fcsr0(void) { uint32_t val; __asm__ __volatile__ (MOVFCSR0_2_GR(%0):"=r"(val)::); return val; }
static inline uint32_t __read_native_fcsr1(void) { uint32_t val; __asm__ __volatile__ (MOVFCSR1_2_GR(%0):"=r"(val)::); return val; }
static inline uint32_t __read_native_fcsr2(void) { uint32_t val; __asm__ __volatile__ (MOVFCSR2_2_GR(%0):"=r"(val)::); return val; }
static inline uint32_t __read_native_fcsr3(void) { uint32_t val; __asm__ __volatile__ (MOVFCSR3_2_GR(%0):"=r"(val)::); return val; }

#else /* !__loongarch__ */

/* Stub implementations for non-LoongArch hosts (offline translation mode) */
static inline void __fld32_native_00(uint64_t *src) { (void)src; }
static inline void __fld32_native_01(uint64_t *src) { (void)src; }
static inline void __fld32_native_02(uint64_t *src) { (void)src; }
static inline void __fld32_native_03(uint64_t *src) { (void)src; }
static inline void __fld32_native_04(uint64_t *src) { (void)src; }
static inline void __fld32_native_05(uint64_t *src) { (void)src; }
static inline void __fld32_native_06(uint64_t *src) { (void)src; }
static inline void __fld32_native_07(uint64_t *src) { (void)src; }
static inline void __fld32_native_08(uint64_t *src) { (void)src; }
static inline void __fld32_native_09(uint64_t *src) { (void)src; }
static inline void __fld32_native_10(uint64_t *src) { (void)src; }
static inline void __fld32_native_11(uint64_t *src) { (void)src; }
static inline void __fld32_native_12(uint64_t *src) { (void)src; }
static inline void __fld32_native_13(uint64_t *src) { (void)src; }
static inline void __fld32_native_14(uint64_t *src) { (void)src; }
static inline void __fld32_native_15(uint64_t *src) { (void)src; }
static inline void __fld32_native_16(uint64_t *src) { (void)src; }
static inline void __fld32_native_17(uint64_t *src) { (void)src; }
static inline void __fld32_native_18(uint64_t *src) { (void)src; }
static inline void __fld32_native_19(uint64_t *src) { (void)src; }
static inline void __fld32_native_20(uint64_t *src) { (void)src; }
static inline void __fld32_native_21(uint64_t *src) { (void)src; }
static inline void __fld32_native_22(uint64_t *src) { (void)src; }
static inline void __fld32_native_23(uint64_t *src) { (void)src; }
static inline void __fld32_native_24(uint64_t *src) { (void)src; }
static inline void __fld32_native_25(uint64_t *src) { (void)src; }
static inline void __fld32_native_26(uint64_t *src) { (void)src; }
static inline void __fld32_native_27(uint64_t *src) { (void)src; }
static inline void __fld32_native_28(uint64_t *src) { (void)src; }
static inline void __fld32_native_29(uint64_t *src) { (void)src; }
static inline void __fld32_native_30(uint64_t *src) { (void)src; }
static inline void __fld32_native_31(uint64_t *src) { (void)src; }

static inline void __fst32_native_00(uint64_t *src) { (void)src; }
static inline void __fst32_native_01(uint64_t *src) { (void)src; }
static inline void __fst32_native_02(uint64_t *src) { (void)src; }
static inline void __fst32_native_03(uint64_t *src) { (void)src; }
static inline void __fst32_native_04(uint64_t *src) { (void)src; }
static inline void __fst32_native_05(uint64_t *src) { (void)src; }
static inline void __fst32_native_06(uint64_t *src) { (void)src; }
static inline void __fst32_native_07(uint64_t *src) { (void)src; }
static inline void __fst32_native_08(uint64_t *src) { (void)src; }
static inline void __fst32_native_09(uint64_t *src) { (void)src; }
static inline void __fst32_native_10(uint64_t *src) { (void)src; }
static inline void __fst32_native_11(uint64_t *src) { (void)src; }
static inline void __fst32_native_12(uint64_t *src) { (void)src; }
static inline void __fst32_native_13(uint64_t *src) { (void)src; }
static inline void __fst32_native_14(uint64_t *src) { (void)src; }
static inline void __fst32_native_15(uint64_t *src) { (void)src; }
static inline void __fst32_native_16(uint64_t *src) { (void)src; }
static inline void __fst32_native_17(uint64_t *src) { (void)src; }
static inline void __fst32_native_18(uint64_t *src) { (void)src; }
static inline void __fst32_native_19(uint64_t *src) { (void)src; }
static inline void __fst32_native_20(uint64_t *src) { (void)src; }
static inline void __fst32_native_21(uint64_t *src) { (void)src; }

static inline void __fld64_native_00(uint64_t *src) { (void)src; }
static inline void __fld64_native_01(uint64_t *src) { (void)src; }
static inline void __fld64_native_02(uint64_t *src) { (void)src; }
static inline void __fld64_native_03(uint64_t *src) { (void)src; }
static inline void __fld64_native_04(uint64_t *src) { (void)src; }
static inline void __fld64_native_05(uint64_t *src) { (void)src; }
static inline void __fld64_native_06(uint64_t *src) { (void)src; }
static inline void __fld64_native_07(uint64_t *src) { (void)src; }
static inline void __fld64_native_08(uint64_t *src) { (void)src; }
static inline void __fld64_native_09(uint64_t *src) { (void)src; }
static inline void __fld64_native_10(uint64_t *src) { (void)src; }
static inline void __fld64_native_11(uint64_t *src) { (void)src; }
static inline void __fld64_native_12(uint64_t *src) { (void)src; }
static inline void __fld64_native_13(uint64_t *src) { (void)src; }
static inline void __fld64_native_14(uint64_t *src) { (void)src; }
static inline void __fld64_native_15(uint64_t *src) { (void)src; }
static inline void __fld64_native_16(uint64_t *src) { (void)src; }
static inline void __fld64_native_17(uint64_t *src) { (void)src; }
static inline void __fld64_native_18(uint64_t *src) { (void)src; }
static inline void __fld64_native_19(uint64_t *src) { (void)src; }
static inline void __fld64_native_20(uint64_t *src) { (void)src; }
static inline void __fld64_native_21(uint64_t *src) { (void)src; }
static inline void __fld64_native_22(uint64_t *src) { (void)src; }
static inline void __fld64_native_23(uint64_t *src) { (void)src; }
static inline void __fld64_native_24(uint64_t *src) { (void)src; }
static inline void __fld64_native_25(uint64_t *src) { (void)src; }
static inline void __fld64_native_26(uint64_t *src) { (void)src; }
static inline void __fld64_native_27(uint64_t *src) { (void)src; }
static inline void __fld64_native_28(uint64_t *src) { (void)src; }
static inline void __fld64_native_29(uint64_t *src) { (void)src; }
static inline void __fld64_native_30(uint64_t *src) { (void)src; }
static inline void __fld64_native_31(uint64_t *src) { (void)src; }

static inline void __fst64_native_00(uint64_t *src) { (void)src; }
static inline void __fst64_native_01(uint64_t *src) { (void)src; }
static inline void __fst64_native_02(uint64_t *src) { (void)src; }
static inline void __fst64_native_03(uint64_t *src) { (void)src; }
static inline void __fst64_native_04(uint64_t *src) { (void)src; }
static inline void __fst64_native_05(uint64_t *src) { (void)src; }
static inline void __fst64_native_06(uint64_t *src) { (void)src; }
static inline void __fst64_native_07(uint64_t *src) { (void)src; }
static inline void __fst64_native_08(uint64_t *src) { (void)src; }
static inline void __fst64_native_09(uint64_t *src) { (void)src; }
static inline void __fst64_native_10(uint64_t *src) { (void)src; }
static inline void __fst64_native_11(uint64_t *src) { (void)src; }
static inline void __fst64_native_12(uint64_t *src) { (void)src; }
static inline void __fst64_native_13(uint64_t *src) { (void)src; }
static inline void __fst64_native_14(uint64_t *src) { (void)src; }
static inline void __fst64_native_15(uint64_t *src) { (void)src; }
static inline void __fst64_native_16(uint64_t *src) { (void)src; }
static inline void __fst64_native_17(uint64_t *src) { (void)src; }
static inline void __fst64_native_18(uint64_t *src) { (void)src; }
static inline void __fst64_native_19(uint64_t *src) { (void)src; }
static inline void __fst64_native_20(uint64_t *src) { (void)src; }
static inline void __fst64_native_21(uint64_t *src) { (void)src; }
static inline void __fst64_native_22(uint64_t *src) { (void)src; }
static inline void __fst64_native_23(uint64_t *src) { (void)src; }
static inline void __fst64_native_24(uint64_t *src) { (void)src; }
static inline void __fst64_native_25(uint64_t *src) { (void)src; }
static inline void __fst64_native_26(uint64_t *src) { (void)src; }
static inline void __fst64_native_27(uint64_t *src) { (void)src; }
static inline void __fst64_native_28(uint64_t *src) { (void)src; }
static inline void __fst64_native_29(uint64_t *src) { (void)src; }
static inline void __fst64_native_30(uint64_t *src) { (void)src; }
static inline void __fst64_native_31(uint64_t *src) { (void)src; }

static inline void __fld32_native_freg_temp(uint64_t *src, int freg) { (void)src; (void)freg; }
static inline void __fst32_native_freg_temp(uint64_t *src, int freg) { (void)src; (void)freg; }
static inline void __fld64_native_freg_temp(uint64_t *src, int freg) { (void)src; (void)freg; }
static inline void __fst64_native_freg_temp(uint64_t *src, int freg) { (void)src; (void)freg; }

static inline void __movgr2fr32_native_00(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_01(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_02(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_03(uint64_t src) { (void)src; }
static inline void __write_native_fcc0(int n) { (void)n; }
static inline void __write_native_fcc1(int n) { (void)n; }
static inline void __write_native_fcc2(int n) { (void)n; }
static inline void __write_native_fcc3(int n) { (void)n; }
static inline void __write_native_fcc4(int n) { (void)n; }
static inline void __write_native_fcc5(int n) { (void)n; }
static inline void __write_native_fcc6(int n) { (void)n; }
static inline void __write_native_fcc7(int n) { (void)n; }

static inline int  __read_native_fcc0(void) { return 0; }
static inline int  __read_native_fcc1(void) { return 0; }
static inline int  __read_native_fcc2(void) { return 0; }
static inline int  __read_native_fcc3(void) { return 0; }
static inline int  __read_native_fcc4(void) { return 0; }
static inline int  __read_native_fcc5(void) { return 0; }
static inline int  __read_native_fcc6(void) { return 0; }
static inline int  __read_native_fcc7(void) { return 0; }

static inline void __write_native_fcsr0(uint32_t fcsr) { (void)fcsr; }
static inline void __write_native_fcsr1(uint32_t fcsr) { (void)fcsr; }
static inline void __write_native_fcsr2(uint32_t fcsr) { (void)fcsr; }
static inline void __write_native_fcsr3(uint32_t fcsr) { (void)fcsr; }

static inline uint32_t __read_native_fcsr0(void) { return 0; }
static inline uint32_t __read_native_fcsr1(void) { return 0; }
static inline uint32_t __read_native_fcsr2(void) { return 0; }
static inline uint32_t __read_native_fcsr3(void) { return 0; }

/* LSX vector register stub functions */
#define STUB_VLD128(n) static inline void __vld128_native_##n(unsigned char *ptr) { (void)ptr; }
#define STUB_VST128(n) static inline void __vst128_native_##n(unsigned char *ptr) { (void)ptr; }

STUB_VLD128(00) STUB_VLD128(01) STUB_VLD128(02) STUB_VLD128(03)
STUB_VLD128(04) STUB_VLD128(05) STUB_VLD128(06) STUB_VLD128(07)
STUB_VLD128(08) STUB_VLD128(09) STUB_VLD128(10) STUB_VLD128(11)
STUB_VLD128(12) STUB_VLD128(13) STUB_VLD128(14) STUB_VLD128(15)
STUB_VLD128(16) STUB_VLD128(17) STUB_VLD128(18) STUB_VLD128(19)
STUB_VLD128(20) STUB_VLD128(21) STUB_VLD128(22) STUB_VLD128(23)
STUB_VLD128(24) STUB_VLD128(25) STUB_VLD128(26) STUB_VLD128(27)
STUB_VLD128(28) STUB_VLD128(29) STUB_VLD128(30) STUB_VLD128(31)

STUB_VST128(00) STUB_VST128(01) STUB_VST128(02) STUB_VST128(03)
STUB_VST128(04) STUB_VST128(05) STUB_VST128(06) STUB_VST128(07)
STUB_VST128(08) STUB_VST128(09) STUB_VST128(10) STUB_VST128(11)
STUB_VST128(12) STUB_VST128(13) STUB_VST128(14) STUB_VST128(15)
STUB_VST128(16) STUB_VST128(17) STUB_VST128(18) STUB_VST128(19)
STUB_VST128(20) STUB_VST128(21) STUB_VST128(22) STUB_VST128(23)
STUB_VST128(24) STUB_VST128(25) STUB_VST128(26) STUB_VST128(27)
STUB_VST128(28) STUB_VST128(29) STUB_VST128(30) STUB_VST128(31)

static inline void __vld128_native_vreg_temp(void *ptr, int vreg) { (void)ptr; (void)vreg; }
static inline void __vst128_native_vreg_temp(void *ptr, int vreg) { (void)ptr; (void)vreg; }

/* Additional stubs for register operations */
static inline void __movgr2fr32_native_freg_temp(uint64_t src, int freg) { (void)src; (void)freg; }
static inline int32_t __movfr2gr32_native_freg_temp(int freg) { (void)freg; return 0; }
static inline void __movgr2fr64_native_freg_temp(uint64_t src, int freg) { (void)src; (void)freg; }
static inline uint64_t __movfr2gr64_native_freg_temp(int freg) { (void)freg; return 0; }

/* Stubs for movgr2fr32 functions */
static inline void __movgr2fr32_native_04(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_05(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_06(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_07(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_08(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_09(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_10(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_11(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_12(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_13(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_14(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_15(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_16(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_17(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_18(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_19(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_20(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_21(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_22(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_23(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_24(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_25(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_26(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_27(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_28(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_29(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_30(uint64_t src) { (void)src; }
static inline void __movgr2fr32_native_31(uint64_t src) { (void)src; }

/* Stubs for movfr2gr32 functions */
static inline int32_t __movfr2gr32_native_00(void) { return 0; }
static inline int32_t __movfr2gr32_native_01(void) { return 0; }
static inline int32_t __movfr2gr32_native_02(void) { return 0; }
static inline int32_t __movfr2gr32_native_03(void) { return 0; }
static inline int32_t __movfr2gr32_native_04(void) { return 0; }
static inline int32_t __movfr2gr32_native_05(void) { return 0; }
static inline int32_t __movfr2gr32_native_06(void) { return 0; }
static inline int32_t __movfr2gr32_native_07(void) { return 0; }
static inline int32_t __movfr2gr32_native_08(void) { return 0; }
static inline int32_t __movfr2gr32_native_09(void) { return 0; }
static inline int32_t __movfr2gr32_native_10(void) { return 0; }
static inline int32_t __movfr2gr32_native_11(void) { return 0; }
static inline int32_t __movfr2gr32_native_12(void) { return 0; }
static inline int32_t __movfr2gr32_native_13(void) { return 0; }
static inline int32_t __movfr2gr32_native_14(void) { return 0; }
static inline int32_t __movfr2gr32_native_15(void) { return 0; }
static inline int32_t __movfr2gr32_native_16(void) { return 0; }
static inline int32_t __movfr2gr32_native_17(void) { return 0; }
static inline int32_t __movfr2gr32_native_18(void) { return 0; }
static inline int32_t __movfr2gr32_native_19(void) { return 0; }
static inline int32_t __movfr2gr32_native_20(void) { return 0; }
static inline int32_t __movfr2gr32_native_21(void) { return 0; }
static inline int32_t __movfr2gr32_native_22(void) { return 0; }
static inline int32_t __movfr2gr32_native_23(void) { return 0; }
static inline int32_t __movfr2gr32_native_24(void) { return 0; }
static inline int32_t __movfr2gr32_native_25(void) { return 0; }
static inline int32_t __movfr2gr32_native_26(void) { return 0; }
static inline int32_t __movfr2gr32_native_27(void) { return 0; }
static inline int32_t __movfr2gr32_native_28(void) { return 0; }
static inline int32_t __movfr2gr32_native_29(void) { return 0; }
static inline int32_t __movfr2gr32_native_30(void) { return 0; }
static inline int32_t __movfr2gr32_native_31(void) { return 0; }

/* Stubs for movgr2fr64 functions */
static inline void __movgr2fr64_native_00(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_01(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_02(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_03(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_04(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_05(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_06(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_07(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_08(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_09(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_10(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_11(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_12(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_13(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_14(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_15(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_16(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_17(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_18(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_19(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_20(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_21(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_22(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_23(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_24(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_25(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_26(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_27(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_28(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_29(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_30(uint64_t src) { (void)src; }
static inline void __movgr2fr64_native_31(uint64_t src) { (void)src; }

/* Stubs for movfr2gr64 functions */
static inline uint64_t __movfr2gr64_native_00(void) { return 0; }
static inline uint64_t __movfr2gr64_native_01(void) { return 0; }
static inline uint64_t __movfr2gr64_native_02(void) { return 0; }
static inline uint64_t __movfr2gr64_native_03(void) { return 0; }
static inline uint64_t __movfr2gr64_native_04(void) { return 0; }
static inline uint64_t __movfr2gr64_native_05(void) { return 0; }
static inline uint64_t __movfr2gr64_native_06(void) { return 0; }
static inline uint64_t __movfr2gr64_native_07(void) { return 0; }
static inline uint64_t __movfr2gr64_native_08(void) { return 0; }
static inline uint64_t __movfr2gr64_native_09(void) { return 0; }
static inline uint64_t __movfr2gr64_native_10(void) { return 0; }
static inline uint64_t __movfr2gr64_native_11(void) { return 0; }
static inline uint64_t __movfr2gr64_native_12(void) { return 0; }
static inline uint64_t __movfr2gr64_native_13(void) { return 0; }
static inline uint64_t __movfr2gr64_native_14(void) { return 0; }
static inline uint64_t __movfr2gr64_native_15(void) { return 0; }
static inline uint64_t __movfr2gr64_native_16(void) { return 0; }
static inline uint64_t __movfr2gr64_native_17(void) { return 0; }
static inline uint64_t __movfr2gr64_native_18(void) { return 0; }
static inline uint64_t __movfr2gr64_native_19(void) { return 0; }
static inline uint64_t __movfr2gr64_native_20(void) { return 0; }
static inline uint64_t __movfr2gr64_native_21(void) { return 0; }
static inline uint64_t __movfr2gr64_native_22(void) { return 0; }
static inline uint64_t __movfr2gr64_native_23(void) { return 0; }
static inline uint64_t __movfr2gr64_native_24(void) { return 0; }
static inline uint64_t __movfr2gr64_native_25(void) { return 0; }
static inline uint64_t __movfr2gr64_native_26(void) { return 0; }
static inline uint64_t __movfr2gr64_native_27(void) { return 0; }
static inline uint64_t __movfr2gr64_native_28(void) { return 0; }
static inline uint64_t __movfr2gr64_native_29(void) { return 0; }
static inline uint64_t __movfr2gr64_native_30(void) { return 0; }
static inline uint64_t __movfr2gr64_native_31(void) { return 0; }

#endif /* __loongarch__ */

#if defined(CONFIG_LATX_OPT_NOFP) && !defined(__loongarch__)
static inline void __nofp_sync_fpr_to_env(CPUState * cpu, int reason) { (void)cpu; (void)reason; }
static inline void __nofp_sync_xmm_to_env(CPUState * cpu) { (void)cpu; }
static inline void __nofp_sync_xmm_from_env(CPUState * cpu) { (void)cpu; }
#endif

#endif
