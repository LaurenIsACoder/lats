/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_ENTRY_FUNC_H_
#define _HAMT_ENTRY_FUNC_H_

extern uint64_t syscall_entry_begin;
extern uint64_t syscall_entry_end;
extern int syscall_entry_size;

extern uint64_t tlb_refill_entry_begin[];
extern uint64_t tlb_refill_entry_end[];
extern int tlb_refill_entry_size[];

extern uint64_t tlb_ldstmy_entry_begin[];
extern uint64_t tlb_ldstmy_entry_end[];
extern int tlb_ldstmy_entry_size[];

extern uint64_t error_tlb_entry_begin[];
extern uint64_t error_tlb_entry_end[];
extern int error_tlb_entry_size[];

extern uint64_t error_stop_entry_begin[];
extern uint64_t error_stop_entry_end[];
extern int error_stop_entry_size[];

extern uint64_t break_entry_begin[];
extern uint64_t break_entry_end[];
extern int break_entry_size[];

extern uint64_t fpe_entry_begin[];
extern uint64_t fpe_entry_end[];
extern int fpe_entry_size[];

extern uint64_t ale_entry_begin[];
extern uint64_t ale_entry_end[];
extern int ale_entry_size[];

#endif
