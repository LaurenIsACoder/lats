/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Run warm before late: the second entry lies inside the first CALL TB,
 * while both CALLs share the return block and its two successors. */
#include "kernels.h"

extern uint64_t alias_leaf(uint64_t);
extern uint64_t warm_entry(uint64_t);
extern uint64_t late_entry(uint64_t);

__asm__(
    ".text\n"
    ".p2align 12\n"
    ".globl alias_leaf\n.type alias_leaf,@function\n"
    "alias_leaf:\n mov %edi,%eax\n and $1,%eax\n ret\n"
    ".size alias_leaf,.-alias_leaf\n"
    ".p2align 4\n"
    ".globl warm_entry\n.type warm_entry,@function\n"
    "warm_entry:\n .byte 0x0f,0x1f,0x00\n"
    ".globl late_entry\n.type late_entry,@function\n"
    "late_entry:\n call alias_leaf\n"
    ".globl alias_return\n"
    "alias_return:\n test %eax,%eax\n jz alias_taken\n"
    ".globl alias_fallthrough\n"
    "alias_fallthrough:\n add $6,%eax\n ret\n"
    ".globl alias_taken\n"
    "alias_taken:\n add $11,%eax\n ret\n"
    ".size late_entry,.-late_entry\n"
    ".size warm_entry,.-warm_entry\n"
);

typedef uint64_t (*entry_fn)(uint64_t);
static entry_fn volatile late_target = late_entry;
static volatile uint64_t leaf_result;

void prepare_data(void)
{
    leaf_result = alias_leaf(42) + alias_leaf(43);
}

__attribute__((noinline))
static uint64_t warm(uint64_t n, uint64_t seed)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n; i++) {
        sum += warm_entry(i ^ seed);
    }
    return sum;
}

__attribute__((noinline))
static uint64_t late(uint64_t n, uint64_t seed)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n; i++) {
        sum += late_target(i ^ seed);
    }
    return sum;
}

const struct kernel kernels[] = {{"warm", warm}, {"late", late}};
const size_t kernel_count = sizeof(kernels) / sizeof(kernels[0]);
