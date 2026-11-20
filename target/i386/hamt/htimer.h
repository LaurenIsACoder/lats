/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HTIMER_H_
#define _HTIMER_H_

#include "hw/i386/apic_internal.h"

void init_timer_mhz(void);
void update_tcfg(APICCommonState *s);
uint32_t get_current_count_from_timertick(APICCommonState *s);
void apic_timer_deliver_from_loongarch(CPUX86State *env);

//#define HTIMER_DEBUG
#ifdef HTIMER_DEBUG
#define HTIMER_LOG(format, ...) \
    fprintf(stderr, "[LATS-HTIMER-LOG]: " format, ##__VA_ARGS__)
#else
#define HTIMER_LOG(format, ...) ((void)0)
#endif

#endif
