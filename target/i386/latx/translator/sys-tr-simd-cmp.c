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

void latxs_sys_simd_cmp_register_ir1(void)
{
    latxs_register_ir1(X86_INS_CMPPD);
    latxs_register_ir1(X86_INS_CMPPS);
    latxs_register_ir1(X86_INS_CMPSS);
}

bool latxs_translate_cmppd(IR1_INST *pir1)
{
    lsassert(ir1_opnd_num(pir1) == 3);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    lsassert(ir1_opnd_is_imm(opnd2));
    uint8 predicate = ir1_opnd_uimm(opnd2) & 0x7;
    switch (predicate) {
    case 0:
        return latxs_translate_cmpeqpd(pir1);
    case 1:
        return latxs_translate_cmpltpd(pir1);
    case 2:
        return latxs_translate_cmplepd(pir1);
    case 3:
        return latxs_translate_cmpunordpd(pir1);
    case 4:
        return latxs_translate_cmpneqpd(pir1);
    case 5:
        return latxs_translate_cmpnltpd(pir1);
    case 6:
        return latxs_translate_cmpnlepd(pir1);
    case 7:
        return latxs_translate_cmpordpd(pir1);
    default:
        lsassert(0);
    }
    return true;
}

bool latxs_translate_cmpps(IR1_INST *pir1)
{
    lsassert(ir1_opnd_num(pir1) == 3);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    lsassert(ir1_opnd_is_imm(opnd2));
    uint8 predicate = ir1_opnd_uimm(opnd2) & 0x7;
    switch (predicate) {
    case 0:
        return latxs_translate_cmpeqps(pir1);
    case 1:
        return latxs_translate_cmpltps(pir1);
    case 2:
        return latxs_translate_cmpleps(pir1);
    case 3:
        return latxs_translate_cmpunordps(pir1);
    case 4:
        return latxs_translate_cmpneqps(pir1);
    case 5:
        return latxs_translate_cmpnltps(pir1);
    case 6:
        return latxs_translate_cmpnleps(pir1);
    case 7:
        return latxs_translate_cmpordps(pir1);
    default:
        lsassert(0);
    }
    return true;
}

bool latxs_translate_cmpss(IR1_INST *pir1)
{
    lsassert(ir1_opnd_num(pir1) == 3);
    IR1_OPND *opnd2 = ir1_get_opnd(pir1, 2);
    lsassert(ir1_opnd_is_imm(opnd2));
    uint8 predicate = ir1_opnd_uimm(opnd2) & 0x7;
    switch (predicate) {
    case 0:
        return latxs_translate_cmpeqss(pir1);
    case 1:
        return latxs_translate_cmpltss(pir1);
    case 2:
        return latxs_translate_cmpless(pir1);
    case 3:
        return latxs_translate_cmpunordss(pir1);
    case 4:
        return latxs_translate_cmpneqss(pir1);
    case 5:
        return latxs_translate_cmpnltss(pir1);
    case 6:
        return latxs_translate_cmpnless(pir1);
    case 7:
        return latxs_translate_cmpordss(pir1);
    default:
        lsassert(0);
    }
    return true;
}
