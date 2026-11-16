/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "cpu.h"
#include "lsenv.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "sys-excp.h"
#include <string.h>

#include "latx-sys-hamt-dmw.h"

void latxs_tr_dmw_in_fix_mem(IR2_OPND *mem)
{
    if (hamt_mode_dmw()) {
        latxs_append_ir2_opnd2i(LISA_LU52I_D, mem, mem, 0x900);
    }
}

void latxs_tr_dmw_out_fix_mem(IR2_OPND *mem)
{
    if (hamt_mode_dmw()) {
        latxs_append_ir2_opnd2i(LISA_LU52I_D, mem, mem, 0x000);
    }
}

/* enter DMW mode : switch ASID to 0xf */
static void enter_guest_asid(IR2_OPND *tmp, IR2_OPND *env)
{
    if (hamt_mode_dmw() && !hamt_disable_asid()) {
        IR2_OPND *zero = &latxs_zero_ir2_opnd;
        if (option_pcid_asid) {
            QEMU_BUILD_BUG_ON(offsetof(CPUX86State, hamt_guest_asid) >= 2048);
            latxs_append_ir2_opnd2i(LISA_LD_HU, tmp, env,
                                    offsetof(CPUX86State, hamt_guest_asid));
        } else {
            latxs_append_ir2_opnd2i(LISA_ORI, tmp, zero, 0xf);
        }
        latxs_append_ir2_opnd2(LISA_CSRWR, &la_csr_asid, tmp);
    }
}

void latxs_tr_dmw_in_fix_asid(IR2_OPND *tmp)
{
    enter_guest_asid(tmp, &latxs_env_ir2_opnd);
}

/* leave DMW mode : switch ASID to 0x0 */
void latxs_tr_dmw_out_fix_asid(IR2_OPND *tmp)
{
    if (hamt_mode_dmw() && !hamt_disable_asid()) {
        IR2_OPND *zero = &latxs_zero_ir2_opnd;
        latxs_append_ir2_opnd2(LISA_CSRWR, &la_csr_asid, zero);
    }
}

void latxs_tr_dmw_in_fix_asid_tmp(IR2_OPND *env)
{
    if (hamt_mode_dmw() && !hamt_disable_asid()) {
        IR2_OPND tmp = latxs_ra_alloc_itemp();
        enter_guest_asid(&tmp, env);
        latxs_ra_free_temp(&tmp);
    }
}

void latxs_tr_dmw_out_fix_asid_tmp(void)
{
    if (hamt_mode_dmw() && !hamt_disable_asid()) {
        IR2_OPND tmp = latxs_ra_alloc_itemp();
        latxs_tr_dmw_out_fix_asid(&tmp);
        latxs_ra_free_temp(&tmp);
    }
}
