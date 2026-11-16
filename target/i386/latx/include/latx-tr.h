/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_TR_H_
#define _LATX_TR_H_

extern int latxs_tr_ready;

static inline void latxs_tr_begin(void *code)
{
    assert(latxs_tr_ready);
    latxs_tr_init(NULL);
}

static inline int latxs_tr_end(void *code)
{
    assert(latxs_tr_ready);
    int ins = latxs_tr_ir2_assemble(code);
    latxs_tr_fini();
    return ins;
}

#endif
