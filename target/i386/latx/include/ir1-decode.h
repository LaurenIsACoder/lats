/*
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef LATXS_IR1_DECODE_H
#define LATXS_IR1_DECODE_H

#include "../capStone/include/capstone/capstone.h"

size_t latxs_decode_one(csh decoder, const uint8_t *code, uint64_t address,
                       int mode, cs_insn **insn, int ir1_num, void *base);

#endif
