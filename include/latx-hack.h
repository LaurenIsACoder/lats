/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_HACK_H_
#define _LATX_HACK_H_

#ifdef CONFIG_LATX_HACK_MEM

#define __HACKRAMBASE        0x7f9000000000ULL
#define __HACKRAMBASE_MASK   0xfff000000000ULL
#define __HACKRAMBASE_TBMASK 0xffff00000000ULL

#define __HACKRAMBASE40         0x9000000000ULL
#define __HACKRAMBASE40_MASK    0xf000000000ULL
#define __HACKRAMBASE40_TBMASK  0xff00000000ULL

#else
#define __HACKRAMBASE  0x0ULL
#define __HACKRAMBASE_MASK 0x0ULL
#define __HACKRAMBASE_TBMASK 0x0ULL
#define __HACKRAMBASE40         0x0ULL
#define __HACKRAMBASE40_MASK    0x0ULL
#define __HACKRAMBASE40_TBMASK  0x0ULL
#endif

extern uint64_t latx_hack_rambase;
extern uint64_t latx_hack_rambase_mask;
extern uint64_t latx_hack_rambase_tbmask;

#ifdef CONFIG_LATX_HACK_MEM
extern uint64_t latx_hack_ram_below_4g_end;
extern uint64_t latx_hack_ram_above_4g_start;
extern uint64_t latx_hack_ram_above_4g_end;

uint64_t latx_hack_mem_backend_extra_size(void);
void latx_hack_mem_set_layout(uint64_t below_4g_end,
                              uint64_t above_4g_start,
                              uint64_t above_4g_size);
#endif

#define HACK_RAMBASE        (latx_hack_rambase)
#define HACK_RAMBASE_MASK   (latx_hack_rambase_mask)
#define HACK_RAMBASE_TBMASK (latx_hack_rambase_tbmask)

#endif
