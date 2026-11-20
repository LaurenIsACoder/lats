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

void latxs_sys_adcx_register_ir1(void)
{
    latxs_register_ir1(X86_INS_ADCX);
    latxs_register_ir1(X86_INS_ADOX);
}

bool latxs_translate_adcx(IR1_INST *pir1)
{
    //IR2_OPND src_opnd = load_ireg_from_ir1(ir1_get_opnd(pir1, 1), ZERO_EXTENSION, false);
    //IR2_OPND dest_opnd = load_ireg_from_ir1(ir1_get_opnd(pir1, 0), ZERO_EXTENSION, false);
    IR2_OPND src_opnd = latxs_ra_alloc_itemp();
    IR2_OPND dest_opnd = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src_opnd, ir1_get_opnd(pir1, 1), EXMode_Z);
    latxs_load_ir1_to_ir2(&dest_opnd, ir1_get_opnd(pir1, 0), EXMode_Z);

    IR2_OPND flag_opnd = latxs_ra_alloc_itemp();
    IR2_OPND cflag_opnd = latxs_ra_alloc_itemp();
    IR2_OPND temp1_opnd = latxs_ra_alloc_itemp();

    //la_x86mfflag(flag_opnd, 0x3f);
    latxs_append_ir2_opnd1i(LISA_X86MFFLAG, &flag_opnd, 0x3f);
    //la_bstrpick_d(cflag_opnd, flag_opnd, 0, 0);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &cflag_opnd, &flag_opnd, 0, 0);

    if (ir1_opnd_size(ir1_get_opnd(pir1, 0)) == 32) {
        //la_add_w(temp1_opnd, dest_opnd, src_opnd);
        latxs_append_ir2_opnd3(LISA_ADD_W, &temp1_opnd, &dest_opnd, &src_opnd);
        //la_add_w(temp1_opnd, temp1_opnd, cflag_opnd);
        latxs_append_ir2_opnd3(LISA_ADD_W, &temp1_opnd, &temp1_opnd, &cflag_opnd);
        //la_x86adc_w(dest_opnd, src_opnd);
        latxs_append_ir2_opnd2(LISA_X86ADC_W, &dest_opnd, &src_opnd);
    } else {
        //la_add_d(temp1_opnd, dest_opnd, src_opnd);
        latxs_append_ir2_opnd3(LISA_ADD_D, &temp1_opnd, &dest_opnd, &src_opnd);
        //la_add_d(temp1_opnd, temp1_opnd, cflag_opnd);
        latxs_append_ir2_opnd3(LISA_ADD_D, &temp1_opnd, &temp1_opnd, &cflag_opnd);
        //la_x86adc_d(dest_opnd, src_opnd);
        latxs_append_ir2_opnd2(LISA_X86ADC_D, &dest_opnd, &src_opnd);
    }
    //la_x86mtflag(flag_opnd, 0x3e); //cf is not recovered
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &flag_opnd, 0x3e);
    
    //store_ireg_to_ir1(temp1_opnd, ir1_get_opnd(pir1, 0), false);
    latxs_store_ir2_to_ir1(&temp1_opnd, ir1_get_opnd(pir1, 0));
    return true;
}

bool latxs_translate_adox(IR1_INST *pir1)
{
    //IR2_OPND src_opnd = load_ireg_from_ir1(ir1_get_opnd(pir1, 1), ZERO_EXTENSION, false);
    //IR2_OPND dest_opnd = load_ireg_from_ir1(ir1_get_opnd(pir1, 0), ZERO_EXTENSION, false);
    IR2_OPND src_opnd = latxs_ra_alloc_itemp();
    IR2_OPND dest_opnd = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src_opnd, ir1_get_opnd(pir1, 1), EXMode_Z);
    latxs_load_ir1_to_ir2(&dest_opnd, ir1_get_opnd(pir1, 0), EXMode_Z);

    IR2_OPND flag_opnd = latxs_ra_alloc_itemp();
    IR2_OPND oflag_opnd = latxs_ra_alloc_itemp();
    IR2_OPND temp1_opnd = latxs_ra_alloc_itemp();
    

    /* save eflags and set of -> cf */
    //la_x86mfflag(flag_opnd, 0x3f);
    latxs_append_ir2_opnd1i(LISA_X86MFFLAG, &flag_opnd, 0x3f);
    //la_bstrpick_d(oflag_opnd, flag_opnd, 11, 11);
    latxs_append_ir2_opnd2ii(LISA_BSTRPICK_D, &oflag_opnd, &flag_opnd, 11, 11);
    //la_x86mtflag(oflag_opnd, 0x1);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &oflag_opnd, 0x1);

    if (ir1_opnd_size(ir1_get_opnd(pir1, 0)) == 32) {
        //la_add_w(temp1_opnd, dest_opnd, src_opnd);
        latxs_append_ir2_opnd3(LISA_ADD_W, &temp1_opnd, &dest_opnd, &src_opnd);
        //la_add_w(temp1_opnd, temp1_opnd, oflag_opnd);
        latxs_append_ir2_opnd3(LISA_ADD_W, &temp1_opnd, &temp1_opnd, &oflag_opnd);
        //la_x86adc_w(dest_opnd, src_opnd);
        latxs_append_ir2_opnd2(LISA_X86ADC_W, &dest_opnd, &src_opnd);
    } else {
        //la_add_d(temp1_opnd, dest_opnd, src_opnd);
        latxs_append_ir2_opnd3(LISA_ADD_D, &temp1_opnd, &dest_opnd, &src_opnd);
        //la_add_d(temp1_opnd, temp1_opnd, oflag_opnd);
        latxs_append_ir2_opnd3(LISA_ADD_D, &temp1_opnd, &temp1_opnd, &oflag_opnd);
        //la_x86adc_d(dest_opnd, src_opnd);
        latxs_append_ir2_opnd2(LISA_X86ADC_D, &dest_opnd, &src_opnd);
    }
    /* set cf -> of */
    //la_x86mfflag(oflag_opnd, 0x1);
    latxs_append_ir2_opnd1i(LISA_X86MFFLAG, &oflag_opnd, 0x1);
    //la_bstrins_d(flag_opnd, oflag_opnd, 11, 11);
    latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &flag_opnd, &oflag_opnd, 11, 11);
    //la_x86mtflag(flag_opnd, 0x3f);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &flag_opnd, 0x3f);

    //store_ireg_to_ir1(temp1_opnd, ir1_get_opnd(pir1, 0), false);
    latxs_store_ir2_to_ir1(&temp1_opnd, ir1_get_opnd(pir1, 0));
    return true;
}
