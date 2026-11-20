/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _IR2_INST_H_
#define _IR2_INST_H_

#include "common.h"
#include "la-ir2.h"
#include "ir2-inst.h"

#define CONFIG_LATX_IR2_COMMENT

typedef struct IR2_INST {
    ADDR _addr;
    int _opcode;
    int _id;
    int op_count;
    IR2_OPND _opnd[4];
    int _b_label_id;
    int _b_ir2_id;
    const char *_b_label_name;
#if defined(CONFIG_LATX_IR2_COMMENT)
    const char *comment;
#endif
} IR2_INST;

inline __attribute__ ((always_inline))
ADDR ir2_addr(IR2_INST *ir2) { return ir2->_addr; }

inline __attribute__ ((always_inline))
IR2_OPCODE ir2_opcode(IR2_INST *ir2) { return (IR2_OPCODE)(ir2->_opcode); }

inline __attribute__ ((always_inline))
int ir2_get_id(IR2_INST *ir2) { return ir2->_id; }

inline __attribute__ ((always_inline))
void ir2_set_addr(IR2_INST *ir2, ADDR a) { ir2->_addr = a; }

inline __attribute__ ((always_inline))
void ir2_set_opcode(IR2_INST *ir2, IR2_OPCODE type) { ir2->_opcode = type; }

inline __attribute__ ((always_inline))
void ir2_set_id(IR2_INST *ir2, int id) { ir2->_id = id; }

#endif
