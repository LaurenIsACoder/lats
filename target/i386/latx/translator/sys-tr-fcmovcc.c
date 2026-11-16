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
#include "flag-lbt.h"
#include "sys-excp.h"

void latxs_sys_fcmovcc_register_ir1(void)
{
    latxs_register_ir1(X86_INS_FCMOVB);
    latxs_register_ir1(X86_INS_FCMOVE);
    latxs_register_ir1(X86_INS_FCMOVBE);
    latxs_register_ir1(X86_INS_FCMOVU);
    latxs_register_ir1(X86_INS_FCMOVNB);
    latxs_register_ir1(X86_INS_FCMOVNE);
    latxs_register_ir1(X86_INS_FCMOVNBE);
    latxs_register_ir1(X86_INS_FCMOVNU);
}

bool latxs_translate_fcmovcc(IR1_INST *pir1)
{
    if (option_soft_fpu) {
        switch (ir1_opcode(pir1)) {
        case X86_INS_FCMOVB:
            return latxs_translate_fcmovb_softfpu(pir1);
        case X86_INS_FCMOVE:
            return latxs_translate_fcmove_softfpu(pir1);
        case X86_INS_FCMOVBE:
            return latxs_translate_fcmovbe_softfpu(pir1);
        case X86_INS_FCMOVU:
            return latxs_translate_fcmovu_softfpu(pir1);
        case X86_INS_FCMOVNB:
            return latxs_translate_fcmovnb_softfpu(pir1);
        case X86_INS_FCMOVNE:
            return latxs_translate_fcmovne_softfpu(pir1);
        case X86_INS_FCMOVNBE:
            return latxs_translate_fcmovnbe_softfpu(pir1);
        case X86_INS_FCMOVNU:
            return latxs_translate_fcmovnu_softfpu(pir1);
        default:
            lsassertm(0, "%s for %s is not implemented\n",
                      __func__, ir1_name(ir1_opcode(pir1)));
            break;
        }
    }
    if (latxs_tr_gen_fp_common_excp_check(pir1) ||
        latxs_tr_gen_excp_check(pir1)) {
        return true;
    }

    IR2_OPND cond = latxs_ra_alloc_itemp();
    latxs_get_eflag_condition(&cond, pir1);

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &cond, &latxs_zero_ir2_opnd,
                                     &label_exit);

    IR2_OPND dst_opnd = latxs_ra_alloc_st(0);
    IR2_OPND src_opnd = latxs_load_freg_from_ir1_1(ir1_get_opnd(pir1, 1),
                                                true);

    latxs_append_ir2_opnd2(LISA_FMOV_D, &dst_opnd, &src_opnd);

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);
    latxs_ra_free_temp(&cond);
    return true;
}
