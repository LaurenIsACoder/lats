/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_PTW_INTERNAL_H_
#define _HAMT_PTW_INTERNAL_H_

#include "hamt-options.h"
#include "hamt-ptw.h"

bool hamt_try_do_ptw(CPUX86State *env,
                     uint64_t badv,
                     void *epc,
                     int w,
                     hamt_ptw_error_t *ptwerror);

#endif
