/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _DUNE_ARCH_H_
#define _DUNE_ARCH_H_

#define PAGESHIFT 14
#define PAGESIZE (1 << PAGESHIFT)

#define NUM_FPU_REGS 32

/**
 * copied from https://github.com/torvalds/linux/tree/master/include/uapi/asm-generic/unistd.h
 */
#define __NR_clone 220
#define __NR_exit 93
#define __NR_kexec_load 104
#define __NR_set_tid_address 96

#define SYS_CLONE __NR_clone
#define SYS_EXIT __NR_exit
#define SYS_KEXEC_LOAD __NR_kexec_load
#define SYS_SET_THREAD_AREA __NR_set_tid_address

#define DUNE_BUILD_ASSERT(cond)             \
do {                                        \
    (void)sizeof(char[1 - 2 * !(cond)]);    \
} while (0)



#define CSR_ECFG_VS_SHIFT 16
#define INSTRUCTION_SIZE 4
#define INT_OFFSET 64
#define VEC_SIZE \
    (1 << (INIT_VALUE_ECFG >> CSR_ECFG_VS_SHIFT)) * INSTRUCTION_SIZE

#endif
