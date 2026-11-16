/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "qemu/osdep.h"
#include "qemu/cacheinfo.h"
#include "cpu.h"
#include "lsenv.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "latx-static-codes.h"
#include "latx-sys-hamt-dmw.h"
#include "latx-config.h"
#include "latx-sys-smc-check.h"
#include "latx-sys-inst-ptn.h"
#include "hamt-data.h"
#include "tcg/tcg-internal.h"
#include "qemu/cacheflush.h"
#include "sys-aot.h"
#include "latx-sys-push-pop.h"

static bool ret_ends_64bit_pushpop_block(const TRANSLATION_DATA *td)
{
    return td->curr_ir1_nr == td->ir1_nr - 1 &&
           td->sys.code64 && (td->sys.flags & HF_CPL_MASK);
}

static bool ret_sp_offsets_fit_signed12(int pending)
{
    /* Keep both the return-address displacement and pending + 8 signed12. */
    return pending >= -2048 && pending <= 2039;
}

static bool ret_is_unfused_near64_without_imm(IR1_INST *ir1)
{
    return ir1_opcode(ir1) == X86_INS_RET && ir1_opnd_num(ir1) == 0 &&
           latxs_ir1_data_size(ir1) != 16 &&
           ir1->instptn.opc == INSTPTN_OPC_NONE &&
           !latxs_ir1_has_prefix_lock(ir1);
}

/* Keep the existing push/pop delta until an ordinary near RET.
 * No new producer is introduced: ADD/SUB and all other barriers are unchanged. */
bool latxs_ret_can_consume_sp(IR1_INST *ir1)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    return option_pushpop && option_pushpop_ret &&
           ret_ends_64bit_pushpop_block(td) &&
           td->sp_patch && ret_sp_offsets_fit_signed12(td->sp_patch) &&
           ret_is_unfused_near64_without_imm(ir1);
}

bool latx_adjust_sp_before_translate(TranslationBlock *tb, IR1_INST *pir1, int index)
{
    if (!option_pushpop)
        return false;

    int *opt = tb_opt_ptr(tb);
    g_assert(opt);
    TRANSLATION_DATA *td = lsenv->tr_data;
    int ir1_nr = td->ir1_nr;
    int opnd_num = ir1_opnd_num(pir1);

    bool clear_esp = false;
    bool update_esp = false;
    bool tb_end = false;

    if ((td->sys.flags & HF_CPL_MASK) &&
        !tb_end &&
        td->sp_patch) {
        IR1_OPND *op;
        int ss_addr_size = latxs_get_sys_stack_addr_size();
        unsigned int id = pir1->info->id;

#ifdef LATXS_INSTPTN_ENABLE
        if (pir1->instptn.opc != INSTPTN_OPC_NONE) {
            /* Last instruction */
            update_esp = true;
            tb_end = true;
        } else
#endif
        if ((index == ir1_nr - 1)) {
            /* Last instruction */
            update_esp = true;
            tb_end = true;
        }
        if (tb_end && latxs_ret_can_consume_sp(pir1)) {
            /* Entry RSP for a fault on RET's return-address load. */
            opt[index] = td->sp_patch;
            return false;
        }
        else if (!tb_end && opnd_num) {
            int j = 0;
            while (j < opnd_num && !update_esp) {
                /* src == esp */
                op = ir1_get_opnd(pir1, j);
                if (ir1_opnd_is_mem(op)) {
                    if (op->mem.base != X86_REG_INVALID) {
                        switch (op->mem.base) {
                            case X86_REG_SP:
                            case X86_REG_ESP:
                            case X86_REG_RSP:
                            case X86_REG_SPL:
                                update_esp = true;
                                break;
                            default:
                                break;
                        }
                    }
                    update_esp = true;
                } else if (ir1_opnd_is_gpr(op)) {
                    switch (op->reg) {
                        case X86_REG_SP:
                        case X86_REG_ESP:
                        case X86_REG_SPL:
                        case X86_REG_RSP: {
                            /* opnd1, opnd2... */
                            if (j) {
                                update_esp = true;
                                break;
                            }
                            /* opnd0 */
                            if (opnd_num == 2 ||
                                id == X86_INS_INC ||
                                id == X86_INS_DEC ||
                                id == X86_INS_NOT ||
                                id == X86_INS_NEG) {
                                update_esp = true;
                            } else {
                                clear_esp = true;
                            }
                            break;
                        }
                        default:
                            break;
                    }
                }
                j++;
            }
        }
        if (update_esp) {
#ifdef CONFIG_LATX_PROFILER
            per_tb_count((void *)&((tb->profile).pushpop_add), 1);
#endif
            latxs_update_stack_pointer(td->sp_patch, ss_addr_size);
            td->sp_patch = 0;
        }
        opt[index] = td->sp_patch;
    }
    return clear_esp;
}

void latx_restore_sp(void)
{
    if (!option_pushpop)
        return;
    TRANSLATION_DATA *td = lsenv->tr_data;
    if (td->sp_patch) {
        int ss_addr_size = latxs_get_sys_stack_addr_size();
#ifdef CONFIG_LATX_PROFILER
        TranslationBlock *tb = lsenv->tr_data->curr_tb;
        per_tb_count((void *)&((tb->profile).pushpop_add), 1);
#endif
        latxs_update_stack_pointer(td->sp_patch, ss_addr_size);
        td->sp_patch = 0;
    }
}
