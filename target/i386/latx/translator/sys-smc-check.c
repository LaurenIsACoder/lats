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
#include "latx-static-codes.h"
#include "latx-sys-hamt-dmw.h"
#include "latx-config.h"
#include "latx-sys-smc-check.h"

#ifdef CONFIG_LATX_SMC_INST_SELF_CHECK

void latxs_tr_gen_smc_inst_self_check(TranslationBlock *tb,
                                      IR1_INST *pir1)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    void *host_pc  = td->host_addr[0];
    void *host_pc2 = td->host_addr[1];

    int iscp = 0;
    int cpboundary = 0;
    if (host_pc2) {
        iscp = 1;
        uint64_t host_pc_next = ((uint64_t)host_pc & ~0xfffULL) + 0x1000ULL;
        cpboundary = host_pc_next - (uint64_t)host_pc;
        /*return;*/
    }

    uint16_t s = pir1->info->size;
    uint8_t *b = pir1->info->bytes;
    uint64_t bimm = 0;

    td->host_addr[0] += s;

    IR2_OPND t0 = la_new_itmp();
    IR2_OPND t1 = la_new_itmp();
    IR2_OPND t2 = la_new_itmp();
    IR2_OPND t3 = la_new_itmp();
    IR2_OPND ok; // label

check_inst_2:

    bimm = 0;
    ok = la_new_label();

    la_imm(&t0, (uint64_t)host_pc);
    if (iscp) la_imm(&t3, (uint64_t)host_pc2);

//    printf("%s:%d S %d isCP %d CP %d Round2 %d\n",
//            __func__, __LINE__,
//            s, iscp, cpboundary, is_check_2);

    switch (s) {
    case 1:
        assert(iscp == 0);
        la_out2i(LISA_LD_BU, &t1, &t0, 0);
        la_out2ii(LISA_BSTRPICK_D, &t1, &t1, 7, 0);
        bimm = bimm | ((uint64_t)(b[0] & 0xff) << 0);
        la_imm(&t2, bimm);
        la_out3 (LISA_BEQ, &t1, &t2, &ok);
        la_outa (LISA_DUP, 0x0);
        la_out1 (LISA_LABEL, &ok);
        break;
    case 2:
        if (iscp) {
            assert(cpboundary == 1);
            la_out2i(LISA_LD_BU,  &t0, &t0, 0);
            la_out2i(LISA_LD_BU,  &t1, &t3, 0);
            la_out2i(LISA_SLLI_D, &t1, &t1, 8);
            la_out3 (LISA_OR,     &t1, &t1, &t0);
        } else {
            la_out2i(LISA_LD_HU, &t1, &t0, 0);
        }
        la_out2ii(LISA_BSTRPICK_D, &t1, &t1, 15, 0);
        bimm = bimm | ((uint64_t)(b[0] & 0xff) << 0);
        bimm = bimm | ((uint64_t)(b[1] & 0xff) << 8);
        la_imm(&t2, bimm);
        la_out3 (LISA_BEQ, &t1, &t2, &ok);
        la_outa (LISA_DUP, 0x0);
        la_out1 (LISA_LABEL, &ok);
        break;
    case 3:
        if (iscp) {
            switch (cpboundary) {
            case 1:
                la_out2i(LISA_LD_BU,  &t0, &t0, 0); // 1
                la_out2i(LISA_LD_HU,  &t1, &t3, 0); // 2
                la_out2i(LISA_SLLI_D, &t1, &t1, 8);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 2:
                la_out2i(LISA_LD_HU,  &t0, &t0, 0); // 2
                la_out2i(LISA_LD_BU,  &t1, &t3, 0); // 1
                la_out2i(LISA_SLLI_D, &t1, &t1, 16);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            default: assert(0); break;
            }
        } else {
            la_out2i(LISA_LD_WU, &t1, &t0, 0);
        }
        la_out2ii(LISA_BSTRPICK_D, &t1, &t1, 23, 0);
        bimm = bimm | ((uint64_t)(b[0] & 0xff) << 0);
        bimm = bimm | ((uint64_t)(b[1] & 0xff) << 8);
        bimm = bimm | ((uint64_t)(b[2] & 0xff) << 16);
        la_imm(&t2, bimm);
        la_out3 (LISA_BEQ, &t1, &t2, &ok);
        la_outa (LISA_DUP, 0x0);
        la_out1 (LISA_LABEL, &ok);
        break;
    case 4:
        if (iscp) {
            switch (cpboundary) {
            case 1:
                la_out2i(LISA_LD_BU,  &t0, &t0, 0); // 1
                la_out2i(LISA_LD_WU,  &t1, &t3, 0); // 3 -> 4
                la_out2i(LISA_SLLI_D, &t1, &t1, 8);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 2:
                la_out2i(LISA_LD_HU,  &t0, &t0, 0); // 2
                la_out2i(LISA_LD_HU,  &t1, &t3, 0); // 2
                la_out2i(LISA_SLLI_D, &t1, &t1, 16);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 3:
                la_out2i(LISA_LD_WU,  &t0, &t0, 0); // 3 -> 4
                la_out2ii(LISA_BSTRPICK_D, &t0, &t0, 23, 0);
                la_out2i(LISA_LD_BU,  &t1, &t3, 0); // 1
                la_out2i(LISA_SLLI_D, &t1, &t1, 24);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            default: assert(0); break;
            }

        } else {
            la_out2i(LISA_LD_WU, &t1, &t0, 0);
        }
        la_out2ii(LISA_BSTRPICK_D, &t1, &t1, 31, 0);
        bimm = bimm | ((uint64_t)(b[0] & 0xff) << 0);
        bimm = bimm | ((uint64_t)(b[1] & 0xff) << 8);
        bimm = bimm | ((uint64_t)(b[2] & 0xff) << 16);
        bimm = bimm | ((uint64_t)(b[3] & 0xff) << 24);
        la_imm(&t2, bimm);
        la_out3 (LISA_BEQ, &t1, &t2, &ok);
        la_outa (LISA_DUP, 0x0);
        la_out1 (LISA_LABEL, &ok);
        break;
    case 5:
        if (iscp) {
            switch (cpboundary) {
            case 1:
                la_out2i(LISA_LD_BU,  &t0, &t0, 0); // 1
                la_out2i(LISA_LD_WU,  &t1, &t3, 0); // 4
                la_out2i(LISA_SLLI_D, &t1, &t1, 8);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 2:
                la_out2i(LISA_LD_HU,  &t0, &t0, 0); // 2
                la_out2i(LISA_LD_WU,  &t1, &t3, 0); // 3 -> 4
                la_out2i(LISA_SLLI_D, &t1, &t1, 16);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 3:
                la_out2i(LISA_LD_WU,  &t0, &t0, 0); // 3 -> 4
                la_out2ii(LISA_BSTRPICK_D, &t0, &t0, 23, 0);
                la_out2i(LISA_LD_HU,  &t1, &t3, 0); // 2
                la_out2i(LISA_SLLI_D, &t1, &t1, 24);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 4:
                la_out2i(LISA_LD_WU,  &t0, &t0, 0); // 4
                la_out2i(LISA_LD_BU,  &t1, &t3, 0); // 1
                la_out2i(LISA_SLLI_D, &t1, &t1, 32);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            default: assert(0); break;
            }
        } else {
            la_out2i(LISA_LD_D, &t1, &t0, 0);
        }
        la_out2ii(LISA_BSTRPICK_D, &t1, &t1, 39, 0);
        bimm = bimm | ((uint64_t)(b[0] & 0xff) << 0);
        bimm = bimm | ((uint64_t)(b[1] & 0xff) << 8);
        bimm = bimm | ((uint64_t)(b[2] & 0xff) << 16);
        bimm = bimm | ((uint64_t)(b[3] & 0xff) << 24);
        bimm = bimm | ((uint64_t)(b[4] & 0xff) << 32);
        la_imm(&t2, bimm);
        la_out3 (LISA_BEQ, &t1, &t2, &ok);
        la_outa (LISA_DUP, 0x0);
        la_out1 (LISA_LABEL, &ok);
        break;
    case 6:
        if (iscp) {
            switch (cpboundary) {
            case 1:
                la_out2i(LISA_LD_BU,  &t0, &t0, 0); // 1
                la_out2i(LISA_LD_D,   &t1, &t3, 0); // 5 -> 8
                la_out2i(LISA_SLLI_D, &t1, &t1, 8);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 2:
                la_out2i(LISA_LD_HU,  &t0, &t0, 0); // 2
                la_out2i(LISA_LD_WU,  &t1, &t3, 0); // 4
                la_out2i(LISA_SLLI_D, &t1, &t1, 16);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 3:
                la_out2i (LISA_LD_WU,  &t0, &t0, 0); // 3 -> 4
                la_out2ii(LISA_BSTRPICK_D, &t0, &t0, 23, 0);
                la_out2i (LISA_LD_WU,  &t1, &t3, 0); // 3 -> 4
                la_out2i (LISA_SLLI_D, &t1, &t1, 24);
                la_out3  (LISA_OR,     &t1, &t1, &t0);
                break;
            case 4:
                la_out2i(LISA_LD_WU,  &t0, &t0, 0); // 4
                la_out2i(LISA_LD_HU,  &t1, &t3, 0); // 2
                la_out2i(LISA_SLLI_D, &t1, &t1, 32);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 5:
                la_out2i (LISA_LD_D,   &t0, &t0, 0); // 5 -> 8
                la_out2ii(LISA_BSTRPICK_D, &t0, &t0, 39, 0);
                la_out2i (LISA_LD_BU,  &t1, &t3, 0); // 1
                la_out2i (LISA_SLLI_D, &t1, &t1, 40);
                la_out3  (LISA_OR,     &t1, &t1, &t0);
                break;
            default: assert(0); break;
            }
        } else {
            la_out2i(LISA_LD_D, &t1, &t0, 0);
        }
        la_out2ii(LISA_BSTRPICK_D, &t1, &t1, 47, 0);
        bimm = bimm | ((uint64_t)(b[0] & 0xff) << 0);
        bimm = bimm | ((uint64_t)(b[1] & 0xff) << 8);
        bimm = bimm | ((uint64_t)(b[2] & 0xff) << 16);
        bimm = bimm | ((uint64_t)(b[3] & 0xff) << 24);
        bimm = bimm | ((uint64_t)(b[4] & 0xff) << 32);
        bimm = bimm | ((uint64_t)(b[5] & 0xff) << 40);
        la_imm(&t2, bimm);
        la_out3 (LISA_BEQ, &t1, &t2, &ok);
        la_outa (LISA_DUP, 0x0);
        la_out1 (LISA_LABEL, &ok);
        break;
    case 7:
        if (iscp) {
            switch (cpboundary) {
            case 1:
                la_out2i(LISA_LD_BU,  &t0, &t0, 0); // 1
                la_out2i(LISA_LD_D,   &t1, &t3, 0); // 6 -> 8
                la_out2i(LISA_SLLI_D, &t1, &t1, 8);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 2:
                la_out2i(LISA_LD_HU,  &t0, &t0, 0); // 2
                la_out2i(LISA_LD_D,   &t1, &t3, 0); // 5 -> 8
                la_out2i(LISA_SLLI_D, &t1, &t1, 16);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 3:
                la_out2i(LISA_LD_WU,  &t0, &t0, 0); // 3 -> 4
                la_out2ii(LISA_BSTRPICK_D, &t0, &t0, 23, 0);
                la_out2i(LISA_LD_WU,  &t1, &t3, 0); // 4
                la_out2i(LISA_SLLI_D, &t1, &t1, 24);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 4:
                la_out2i(LISA_LD_WU,  &t0, &t0, 0); // 4
                la_out2i(LISA_LD_WU,  &t1, &t3, 0); // 3 -> 4
                la_out2i(LISA_SLLI_D, &t1, &t1, 32);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 5:
                la_out2i(LISA_LD_D,   &t0, &t0, 0); // 5 -> 8
                la_out2ii(LISA_BSTRPICK_D, &t0, &t0, 39, 0);
                la_out2i(LISA_LD_HU,  &t1, &t3, 0); // 2
                la_out2i(LISA_SLLI_D, &t1, &t1, 40);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 6:
                la_out2i(LISA_LD_D,   &t0, &t0, 0); // 6 -> 8
                la_out2ii(LISA_BSTRPICK_D, &t0, &t0, 47, 0);
                la_out2i(LISA_LD_BU,  &t1, &t3, 0); // 1
                la_out2i(LISA_SLLI_D, &t1, &t1, 48);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            default: assert(0); break;
            }
        } else {
            la_out2i(LISA_LD_D, &t1, &t0, 0);
        }
        la_out2ii(LISA_BSTRPICK_D, &t1, &t1, 55, 0);
        bimm = bimm | ((uint64_t)(b[0] & 0xff) << 0);
        bimm = bimm | ((uint64_t)(b[1] & 0xff) << 8);
        bimm = bimm | ((uint64_t)(b[2] & 0xff) << 16);
        bimm = bimm | ((uint64_t)(b[3] & 0xff) << 24);
        bimm = bimm | ((uint64_t)(b[4] & 0xff) << 32);
        bimm = bimm | ((uint64_t)(b[5] & 0xff) << 40);
        bimm = bimm | ((uint64_t)(b[6] & 0xff) << 48);
        la_imm(&t2, bimm);
        la_out3 (LISA_BEQ, &t1, &t2, &ok);
        la_outa (LISA_DUP, 0x0);
        la_out1 (LISA_LABEL, &ok);
        break;
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
        if (iscp) {
            switch (cpboundary) {
            case 1:
                la_out2i(LISA_LD_BU,  &t0, &t0, 0); // 1
                la_out2i(LISA_LD_D,   &t1, &t3, 0); // 7 -> 8
                la_out2i(LISA_SLLI_D, &t1, &t1, 8);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 2:
                la_out2i(LISA_LD_HU,  &t0, &t0, 0); // 2
                la_out2i(LISA_LD_D,   &t1, &t3, 0); // 6 -> 8
                la_out2i(LISA_SLLI_D, &t1, &t1, 16);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 3:
                la_out2i(LISA_LD_WU,  &t0, &t0, 0); // 3 -> 4
                la_out2ii(LISA_BSTRPICK_D, &t0, &t0, 23, 0);
                la_out2i(LISA_LD_D,   &t1, &t3, 0); // 5
                la_out2i(LISA_SLLI_D, &t1, &t1, 24);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 4:
                la_out2i(LISA_LD_WU,  &t0, &t0, 0); // 4
                la_out2i(LISA_LD_WU,  &t1, &t3, 0); // 4
                la_out2i(LISA_SLLI_D, &t1, &t1, 32);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 5:
                la_out2i(LISA_LD_D,   &t0, &t0, 0); // 5 -> 8
                la_out2ii(LISA_BSTRPICK_D, &t0, &t0, 39, 0);
                la_out2i(LISA_LD_WU,  &t1, &t3, 0); // 3 -> 4
                la_out2i(LISA_SLLI_D, &t1, &t1, 40);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 6:
                la_out2i(LISA_LD_D,   &t0, &t0, 0); // 6 -> 8
                la_out2ii(LISA_BSTRPICK_D, &t0, &t0, 47, 0);
                la_out2i(LISA_LD_HU,  &t1, &t3, 0); // 2
                la_out2i(LISA_SLLI_D, &t1, &t1, 48);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 7:
                la_out2i(LISA_LD_D,   &t0, &t0, 0); // 7 -> 8
                la_out2ii(LISA_BSTRPICK_D, &t0, &t0, 55, 0);
                la_out2i(LISA_LD_BU,  &t1, &t3, 0); // 1
                la_out2i(LISA_SLLI_D, &t1, &t1, 56);
                la_out3 (LISA_OR,     &t1, &t1, &t0);
                break;
            case 8:  assert(s > 8);  goto docppage1ld8;
            case 9:  assert(s > 9);  goto docppage1ld8;
            case 10: assert(s > 10); goto docppage1ld8;
            case 11: assert(s > 11); goto docppage1ld8;
            case 12: assert(s > 12); goto docppage1ld8;
            case 13: assert(s > 13); goto docppage1ld8;
            case 14: assert(s > 14);
docppage1ld8:
                la_out2i(LISA_LD_D, &t1, &t0, 0);
                break;
            default: assert(0); break;
            }
        } else {
            la_out2i(LISA_LD_D, &t1, &t0, 0);
        }
        bimm = bimm | ((uint64_t)(b[0] & 0xff) << 0);
        bimm = bimm | ((uint64_t)(b[1] & 0xff) << 8);
        bimm = bimm | ((uint64_t)(b[2] & 0xff) << 16);
        bimm = bimm | ((uint64_t)(b[3] & 0xff) << 24);
        bimm = bimm | ((uint64_t)(b[4] & 0xff) << 32);
        bimm = bimm | ((uint64_t)(b[5] & 0xff) << 40);
        bimm = bimm | ((uint64_t)(b[6] & 0xff) << 48);
        bimm = bimm | ((uint64_t)(b[7] & 0xff) << 56);
        la_imm(&t2, bimm);
        la_out3 (LISA_BEQ, &t1, &t2, &ok);
        la_outa (LISA_DUP, 0x0);
        la_out1 (LISA_LABEL, &ok);
        if (s != 8) {
            if (iscp) {
                /*printf("%s:%d S %d CP %d\n", __func__, __LINE__, s, cpboundary);*/
                if (cpboundary <= 8) {
                    iscp = 0;
                    host_pc = host_pc2 + (8 - cpboundary);
                    s -= 8;
                    b += 8;
                    goto check_inst_2;
                } else {
                    host_pc = host_pc + 8;
                    cpboundary -= 8;
                    s -= 8;
                    b += 8;
                    goto check_inst_2;
                }
            } else {
                /*printf("%s:%d S %d\n", __func__, __LINE__, s);*/
                host_pc = host_pc + 8;
                s -= 8;
                b += 8;
                goto check_inst_2;
            }
        }
        break;
    default:
        assert(0);
        break;
    }

    la_tmp_free(t0);
    la_tmp_free(t1);
    la_tmp_free(t2);
    la_tmp_free(t3);
}

#endif
