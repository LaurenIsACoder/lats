/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_IOCSR_H_
#define _HAMT_IOCSR_H_

#define LOONGARCH_IOCSR_IPI_STATUS  0x1000
#define LOONGARCH_IOCSR_IPI_EN      0x1004
#define LOONGARCH_IOCSR_IPI_SET     0x1008
#define LOONGARCH_IOCSR_IPI_CLEAR   0x100c
#define LOONGARCH_IOCSR_MBUF0       0x1020
#define LOONGARCH_IOCSR_MBUF1       0x1028
#define LOONGARCH_IOCSR_MBUF2       0x1030
#define LOONGARCH_IOCSR_MBUF3       0x1038

#define LOONGARCH_IOCSR_IPI_SEND    0x1040
#define  IOCSR_IPI_SEND_IP_SHIFT    0
#define  IOCSR_IPI_SEND_CPU_SHIFT   16
#define  IOCSR_IPI_SEND_BLOCKING    (1<<31)

#define LOONGARCH_IOCSR_MBUF_SEND   0x1048
#define  IOCSR_MBUF_SEND_BLOCKING   (1<<31)
#define  IOCSR_MBUF_SEND_BOX_SHIFT  2
#define  IOCSR_MBUF_SEND_BOX_LO(box)    (box << 1)
#define  IOCSR_MBUF_SEND_BOX_HI(box)    ((box << 1) + 1)
#define  IOCSR_MBUF_SEND_CPU_SHIFT  16
#define  IOCSR_MBUF_SEND_BUF_SHIFT  32
#define  IOCSR_MBUF_SEND_H32_MASK   0xFFFFFFFF00000000ULL

#define LOONGARCH_IOCSR_ANY_SEND    0x1158
#define  IOCSR_ANY_SEND_BLOCKING    (1<<31)
#define  IOCSR_ANY_SEND_CPU_SHIFT   16
#define  IOCSR_ANY_SEND_MASK_SHIFT  27
#define  IOCSR_ANY_SEND_BUF_SHIFT   32
#define  IOCSR_ANY_SEND_H32_MASK    0xFFFFFFFF00000000ULL

static inline uint32_t iocsr_read32(uint32_t reg)
{
    uint32_t val;
    asm volatile (
        "iocsrrd.w %[val], %[reg] \n\t"
        : [val] "=r" (val)
        : [reg] "r" (reg)
        : "memory");
    return val;
}

static inline uint64_t iocsr_read64(uint32_t reg)
{
    uint64_t val;
    asm volatile (
        "iocsrrd.d %[val], %[reg] \n\t"
        : [val] "=r" (val)
        : [reg] "r" (reg)
        : "memory");
    return val;
}

static inline void iocsr_write8(uint8_t val, uint32_t reg)
{
    asm volatile (
        "iocsrwr.b %[val], %[reg] \n\t"
        :
        : [val] "r" (val), [reg] "r" (reg)
        : "memory");
}

static inline void iocsr_write32(uint32_t val, uint32_t reg)
{
    asm volatile (
        "iocsrwr.w %[val], %[reg] \n\t"
        :
        : [val] "r" (val), [reg] "r" (reg)
        : "memory");
}

static inline void iocsr_write64(uint64_t val, uint32_t reg)
{
    asm volatile (
        "iocsrwr.d %[val], %[reg] \n\t"
        :
        : [val] "r" (val), [reg] "r" (reg)
        : "memory");
}

#endif
