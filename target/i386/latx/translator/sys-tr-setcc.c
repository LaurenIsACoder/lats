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
#include "latx-sys-tr-mmio.h"

void latxs_sys_setcc_register_ir1(void)
{
    latxs_register_ir1(X86_INS_SETE);
    latxs_register_ir1(X86_INS_SETNE);
    latxs_register_ir1(X86_INS_SETO);
    latxs_register_ir1(X86_INS_SETNO);
    latxs_register_ir1(X86_INS_SETB);
    latxs_register_ir1(X86_INS_SETAE);
    latxs_register_ir1(X86_INS_SETBE);
    latxs_register_ir1(X86_INS_SETA);
    latxs_register_ir1(X86_INS_SETS);
    latxs_register_ir1(X86_INS_SETNS);
    latxs_register_ir1(X86_INS_SETP);
    latxs_register_ir1(X86_INS_SETNP);
    latxs_register_ir1(X86_INS_SETL);
    latxs_register_ir1(X86_INS_SETGE);
    latxs_register_ir1(X86_INS_SETLE);
    latxs_register_ir1(X86_INS_SETG);

    latxs_register_ir1(X86_INS_BSF);
    latxs_register_ir1(X86_INS_BSR);
    latxs_register_ir1(X86_INS_BT);
    latxs_register_ir1(X86_INS_BTC);
    latxs_register_ir1(X86_INS_BTR);
    latxs_register_ir1(X86_INS_BTS);

    latxs_register_ir1(X86_INS_LZCNT);
}

/**
 * @brief SETcc translation
 *
 * SETcc has:
 * - SETO    // OF = 1
 * - SETNO   // OF = 0
 * - SETB    // CF = 1
 * - SETAE   // CF = 0
 * - SETE    // ZF = 1
 * - SETNE   // ZF = 0
 * - SETBE   // CF = 1 | ZF = 1
 * - SETA    // CF = O & ZF = O
 * - SETS    // SF = 1
 * - SETNS   // SF = 0
 * - SETP    // PF = 1
 * - SETNP   // PF = 0
 * - SETL    // SF <> OF
 * - SETGE   // SF = OF
 * - SETLE   // ZF = 1 | SF <> OF
 * - SETG    // ZF = 0 & SF = OF
 *
 * @param pir1 input current inst
 * @return true if transalte success
 * @return false translate failure
 */

bool latxs_translate_setcc(IR1_INST *pir1)
{
    IR2_OPND value_opnd = latxs_ra_alloc_itemp();

    latxs_get_eflag_condition(&value_opnd, pir1);

    latxs_store_ir2_to_ir1(&value_opnd, ir1_get_opnd(pir1, 0));
    latxs_ra_free_temp(&value_opnd);

    return true;
}

bool latxs_translate_bsf(IR1_INST *pir1)
{
    if (ir1_opcode(pir1) == X86_INS_TZCNT) {
        return latxs_translate_tzcnt(pir1);
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND *zero = &latxs_zero_ir2_opnd;

    IR2_OPND src = latxs_ra_alloc_itemp();

    latxs_load_ir1_to_ir2(&src, opnd1, EXMode_Z);

    if (ir1_is_zf_def(pir1)) {
        IR2_OPND tmp = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_ORI, &tmp, zero, 0xfff);
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &tmp, 0x8);
        latxs_ra_free_temp(&tmp);
    }

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &src, zero, &label_exit);

    if (ir1_opnd_size(opnd0) == 64) {
        IR2_OPND gpr = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2(LISA_CTZ_D, &gpr, &src);
    } else {
        IR2_OPND res = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2(LISA_CTZ_D, &res, &src);
        latxs_store_ir2_to_ir1(&res, opnd0);
    }

    if (ir1_is_zf_def(pir1)) {
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, zero, 0x8);
    }

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    return true;
}

bool latxs_translate_bsr(IR1_INST *pir1)
{
    if (pir1->info->bytes[0] == 0xF3) {
        pir1->info->id = X86_INS_LZCNT;
        latxs_translate_lzcnt(pir1);
        return true;
    }

    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0);
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1);

    IR2_OPND *zero = &latxs_zero_ir2_opnd;

    IR2_OPND res = latxs_ra_alloc_itemp();
    IR2_OPND src = latxs_ra_alloc_itemp();

    latxs_load_ir1_to_ir2(&src, opnd1, EXMode_Z);

    if (ir1_is_zf_def(pir1)) {
        IR2_OPND tmp = latxs_ra_alloc_itemp();
        latxs_append_ir2_opnd2i(LISA_ORI, &tmp, zero, 0xfff);
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &tmp, 0x8);
        latxs_ra_free_temp(&tmp);
    }

    IR2_OPND label_exit = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd3(LISA_BEQ, &src, zero, &label_exit);

    if (ir1_opnd_size(opnd0) == 64) {
        IR2_OPND gpr = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2( LISA_CLZ_D,  &gpr, &src);
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &gpr, &gpr, -63);
        latxs_append_ir2_opnd3( LISA_SUB_D,  &gpr, zero, &gpr);
    } else {
        latxs_append_ir2_opnd2( LISA_CLZ_D,  &res, &src);
        latxs_append_ir2_opnd2i(LISA_ADDI_D, &res, &res, -63);
        latxs_append_ir2_opnd3( LISA_SUB_D,  &res, zero, &res);
        latxs_store_ir2_to_ir1(&res, opnd0);
    }

    if (ir1_is_zf_def(pir1)) {
        latxs_append_ir2_opnd1i(LISA_X86MTFLAG, zero, 0x8);
    }

    latxs_append_ir2_opnd1(LISA_LABEL, &label_exit);

    return true;
}

bool latxs_translate_btx(IR1_INST *pir1)
{
#ifdef CONFIG_SOFTMMU
    if (latxs_translate_bt_opt_apic(pir1)) {
        return true;
    }
#endif
    if (latxs_ir1_has_prefix_lock(pir1) &&
        (lsenv->tr_data->sys.cflags & CF_PARALLEL) &&
        ir1_opcode(pir1) != X86_INS_BT) {
        return latxs_translate_lock_btx(pir1);
    }
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0); /* bit base   : GPR/MEM */
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1); /* bit offset : GPR/imm */

    int t_imm = 0;
    int t_addr_shift = 0;
    switch (ir1_opnd_size(opnd0)) {
    case 16:
        t_imm = 0xf;
        t_addr_shift = 1;
        break;
    case 32:
        t_imm = 0x1f;
        t_addr_shift = 2;
        break;
    case 64:
        t_imm = 0x3f;
        t_addr_shift = 3;
        break;
    default:
        lsassertm(0, "Invalid operand size (%d) in translate_btx.\n",
                ir1_opnd_size(opnd0));
        break;
    }

    IR2_OPND bit_base = latxs_ra_alloc_itemp();
    IR2_OPND bit_off = latxs_ra_alloc_itemp();
    IR2_OPND mem_base_load;
    IR2_OPND mem_base;
    int mem_offset;

    /* get bit_base and bit_off */
    if (ir1_opnd_is_gpr(opnd0)) {
        latxs_load_ir1_to_ir2(&bit_base, opnd0, EXMode_Z);
        latxs_load_ir1_to_ir2(&bit_off,  opnd1, EXMode_Z);
        latxs_append_ir2_opnd2i(LISA_ANDI, &bit_off, &bit_off, t_imm);
    } else {
        /* opnd0 is memory operand */
        if (ir1_opnd_is_imm(opnd1)) {
            latxs_load_ir1_to_ir2(&bit_off, opnd1, EXMode_Z);
            latxs_append_ir2_opnd2i(LISA_ANDI, &bit_off, &bit_off, t_imm);
        } else {
            latxs_load_ir1_to_ir2(&bit_off, opnd1, EXMode_S);
        }

        latxs_convert_mem_opnd(&mem_base_load, &mem_offset, opnd0, -1);
        if (mem_offset != 0) {
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &mem_base_load, &mem_base_load, mem_offset);
#ifdef LATXS_IMM_CACHE_ENABLE
            imm_cache_addi_update(mem_offset);
#endif
        }

        mem_base = latxs_ra_alloc_itemp();

        latxs_append_ir2_opnd2i(LISA_SRAI_D, &mem_base, &bit_off, 3 + t_addr_shift);
        latxs_append_ir2_opnd2i(LISA_SLLI_D, &mem_base, &mem_base, t_addr_shift);
        latxs_append_ir2_opnd2i(LISA_ANDI, &bit_off, &bit_off, t_imm);

        latxs_append_ir2_opnd3(LISA_ADD_D, &mem_base, &mem_base_load, &mem_base);

        int addr_size = latxs_ir1_addr_size(pir1);
        if (addr_size == 4) {
            latxs_append_ir2_opnd2_(lisa_mov32z, &mem_base, &mem_base);
        } else if (addr_size == 2) {
            latxs_append_ir2_opnd2_(lisa_mov16z, &mem_base, &mem_base);
        }

        switch (ir1_opnd_size(opnd0)) {
        case 16:
            gen_ldst_softmmu_helper(LISA_LD_HU, &bit_base, &mem_base, 0, 1);
            break;
        case 32:
            gen_ldst_softmmu_helper(LISA_LD_WU, &bit_base, &mem_base, 0, 1);
            break;
        case 64:
            gen_ldst_softmmu_helper(LISA_LD_D, &bit_base, &mem_base, 0, 1);
            break;
        default:
            lsassertm(0, "Invalid operand size (%d) in translate_btx.\n",
                    ir1_opnd_size(opnd0));
            break;
        }
    }

    IR2_OPND bit_eflags = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd3(LISA_SRL_D, &bit_eflags, &bit_base, &bit_off);

    /* update BitBase */
    IR2_OPND bit_set = latxs_ra_alloc_itemp();
    switch (ir1_opcode(pir1)) {
    case X86_INS_BT:
        break;
    case X86_INS_BTS: /* set bit = 1 */
        latxs_append_ir2_opnd2i(LISA_ORI, &bit_set, &latxs_zero_ir2_opnd, 1);
        latxs_append_ir2_opnd3(LISA_SLL_D, &bit_set, &bit_set, &bit_off);
        latxs_append_ir2_opnd3(LISA_OR, &bit_base, &bit_base, &bit_set);
        break;
    case X86_INS_BTR: /* set bit = 0 */
        latxs_append_ir2_opnd2i(LISA_ORI, &bit_set, &latxs_zero_ir2_opnd, 1);
        latxs_append_ir2_opnd3(LISA_SLL_D, &bit_set, &bit_set, &bit_off);
        latxs_append_ir2_opnd2_(lisa_not, &bit_set, &bit_set);
        latxs_append_ir2_opnd3(LISA_AND, &bit_base, &bit_base, &bit_set);
        break;
    case X86_INS_BTC: /* set bit = not bit  */
        latxs_append_ir2_opnd2i(LISA_ORI, &bit_set, &latxs_zero_ir2_opnd, 1);
        latxs_append_ir2_opnd3(LISA_SLL_D, &bit_set, &bit_set, &bit_off);
        latxs_append_ir2_opnd3(LISA_XOR, &bit_base, &bit_base, &bit_set);
        break;
    default:
        lsassertm(0, "Invalid opcode in translate_btx\n");
        break;
    }
    latxs_ra_free_temp(&bit_set);
    latxs_ra_free_temp(&bit_off);

    if (ir1_opcode(pir1) != X86_INS_BT) {
        if (ir1_opnd_is_gpr(opnd0)) {
            latxs_store_ir2_to_ir1(&bit_base, opnd0);
        } else {
            switch (ir1_opnd_size(opnd0)) {
            case 16:
                gen_ldst_softmmu_helper(LISA_ST_H, &bit_base, &mem_base, 0, 1);
                break;
            case 32:
                gen_ldst_softmmu_helper(LISA_ST_W, &bit_base, &mem_base, 0, 1);
                break;
            case 64:
                gen_ldst_softmmu_helper(LISA_ST_D, &bit_base, &mem_base, 0, 1);
                break;
            default:
                lsassertm(0, "Invalid operand size (%d) in translate_btx.\n",
                        ir1_opnd_size(opnd0));
                break;
            }
        }
    }
    latxs_ra_free_temp(&bit_base);

    /* update EFALGS */
    latxs_append_ir2_opnd2i(LISA_ANDI, &bit_eflags, &bit_eflags, 1);
    latxs_append_ir2_opnd1i(LISA_X86MTFLAG, &bit_eflags, 0x1);
    latxs_ra_free_temp(&bit_eflags);

    return true;
}

bool latxs_translate_lzcnt(IR1_INST *pir1)
{
    // IR2_OPND src_opnd =
    //     load_ireg_from_ir1(ir1_get_opnd(pir1, 1), ZERO_EXTENSION, false);
    IR2_OPND src_opnd = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&src_opnd, ir1_get_opnd(pir1, 1), EXMode_Z);

    IR2_OPND temp = latxs_ra_alloc_itemp();
    IR2_OPND count;
    bool count_opnd_is_temp = false;
    int opnd_size = ir1_opnd_size(ir1_get_opnd(pir1, 0));
    //li_d(temp, 64 - opnd_size);
    latxs_load_imm64_to_ir2(&temp, 64 - opnd_size);
    lsassert(ir1_opnd_is_gpr(ir1_get_opnd(pir1, 0)));
    if (opnd_size == 64) {
        count = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(ir1_get_opnd(pir1, 0)));
    } else {
        count = latxs_ra_alloc_itemp();
        count_opnd_is_temp = true;
    }

    //la_clz_d(count, src_opnd);
    latxs_append_ir2_opnd2(LISA_CLZ_D, &count, &src_opnd);
    //la_sub_d(count, count, temp);
    latxs_append_ir2_opnd3(LISA_SUB_D, &count, &count, &temp);
    if (count_opnd_is_temp) {
        latxs_store_ir2_to_ir1(&count, ir1_get_opnd(pir1, 0));
        latxs_ra_free_temp(&count);
    }

    // IR2_OPND dest_opnd =
    //     load_ireg_from_ir1(ir1_get_opnd(pir1, 0), ZERO_EXTENSION, false);
    IR2_OPND dest_opnd = latxs_ra_alloc_itemp();
    latxs_load_ir1_to_ir2(&dest_opnd, ir1_get_opnd(pir1, 0), EXMode_Z);
    //generate_eflag_calculation(count, count, src_opnd, pir1, true);
    latxs_generate_eflag_calculation(&dest_opnd, &dest_opnd, &src_opnd, pir1, true);

    return true;
}
