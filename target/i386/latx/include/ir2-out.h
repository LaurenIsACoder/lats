/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_IR2_OUT_H_
#define _LATX_IR2_OUT_H_

#define la_out3i(opc, o0, o1, o2, i)   latxs_append_ir2_opnd3i(opc, o0, o1, o2, i)
#define la_out1ii(opc, o0, i1, i2)     latxs_append_ir2_opnd1ii(opc, o0, i1, ii)
#define la_out2ii(opc, o0, o1, i1, i2) latxs_append_ir2_opnd2ii(opc, o0, o1, i1, i2)
#define la_out3(opc, o0, o1, o2)       latxs_append_ir2_opnd3(opc, o0, o1, o2)
#define la_out4(opc, o0, o1, o2, o3)   latxs_append_ir2_opnd4(opc, o0, o1, o2, o3)
#define la_out2i(opc, o0, o1, i)       latxs_append_ir2_opnd2i(opc, o0, o1, i)
#define la_out2(opc, o0, o1)           latxs_append_ir2_opnd2(opc, o0, o1)
#define la_out1i(opc, o, i)            latxs_append_ir2_opnd1i(opc, o, i)
#define la_out1(opc, o)                latxs_append_ir2_opnd1(opc, o)
#define la_outi(opc, i)                latxs_append_ir2_opndi(opc, i)
#define la_outa(opc, a)                latxs_append_ir2_opnda(opc, a)
#define la_out0(opc)                   latxs_append_ir2_opnd0(opc)
#define la_out2_(opc, o0, o1)          latxs_append_ir2_opnd2_(opc, o0, o1)
#define la_out1_(opc, o)               latxs_append_ir2_opnd1_(opc, o)
#define la_out0_(opc)                  latxs_append_ir2_opnd0_(opc)

#define la_out_nop()    latxs_append_ir2_opnd0_(lisa_nop)
#define la_label(l)     latxs_append_label(&(l))
#define la_label_ptr(l)     latxs_append_label((l))
#define la_label_name(l, n)    latxs_append_label_with_name(&(l), n)
#define la_imm(o, imm)  latxs_load_imm64(o, imm)

#define la_code(c)      latxs_ir2_opnd_new(IR2_OPND_CODE, (c))

#define la_scr0 latxs_scr0_ir2_opnd
#define la_scr1 latxs_scr1_ir2_opnd
#define la_scr2 latxs_scr2_ir2_opnd
#define la_scr3 latxs_scr3_ir2_opnd

#define la_zero latxs_zero_ir2_opnd
#define la_sp   latxs_sp_ir2_opnd
#define la_fp   latxs_fp_ir2_opnd
#define la_ra   latxs_ra_ir2_opnd
#define la_a0   latxs_arg0_ir2_opnd
#define la_a1   latxs_arg1_ir2_opnd
#define la_a2   latxs_arg2_ir2_opnd
#define la_a3   latxs_arg3_ir2_opnd
#define la_a4   latxs_arg4_ir2_opnd
#define la_a5   latxs_arg5_ir2_opnd
#define la_a6   latxs_arg6_ir2_opnd
#define la_a7   latxs_arg7_ir2_opnd
#define la_v0   latxs_ret0_ir2_opnd
#define la_v1   latxs_ret1_ir2_opnd

#define la_t0   latxs_tmp0_ir2_opnd
#define la_t1   latxs_tmp1_ir2_opnd
#define la_t2   latxs_tmp2_ir2_opnd
#define la_t3   latxs_tmp3_ir2_opnd
#define la_t4   latxs_tmp4_ir2_opnd
#define la_t5   latxs_tmp5_ir2_opnd
#define la_t6   latxs_tmp6_ir2_opnd
#define la_t7   latxs_tmp7_ir2_opnd

#define la_s0   latxs_s0_ir2_opnd
#define la_s1   latxs_s1_ir2_opnd
#define la_s2   latxs_s2_ir2_opnd
#define la_s3   latxs_s3_ir2_opnd
#define la_s4   latxs_s4_ir2_opnd
#define la_s5   latxs_s5_ir2_opnd
#define la_s6   latxs_s6_ir2_opnd
#define la_s7   latxs_s7_ir2_opnd
#define la_s8   latxs_s8_ir2_opnd

#define la_fcsr     latxs_fcsr_ir2_opnd
#define la_fcsr1    latxs_fcsr1_ir2_opnd
#define la_fcsr2    latxs_fcsr2_ir2_opnd
#define la_fcsr3    latxs_fcsr3_ir2_opnd
#define la_fcc0     latxs_fcc0_ir2_opnd
#define la_fcc1     latxs_fcc1_ir2_opnd
#define la_fcc2     latxs_fcc2_ir2_opnd
#define la_fcc3     latxs_fcc3_ir2_opnd
#define la_fcc4     latxs_fcc4_ir2_opnd
#define la_fcc5     latxs_fcc5_ir2_opnd
#define la_fcc6     latxs_fcc6_ir2_opnd
#define la_fcc7     latxs_fcc7_ir2_opnd

#define la_inv      latxs_invalid_ir2_opnd
#define la_env      latxs_env_ir2_opnd

#define la_new_gpr(n)   latxs_ir2_opnd_new(IR2_OPND_GPR, (n))
#define la_new_itmp()   latxs_ra_alloc_itemp()
#define la_new_ftmp()   latxs_ra_alloc_ftemp()
#define la_new_label()  latxs_ir2_opnd_new_label()
#define la_tmp_free(t)  latxs_ra_free_temp(&(t))

#include "ir2-comment.h"

#endif
