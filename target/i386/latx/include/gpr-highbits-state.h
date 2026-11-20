/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef LATX_GPR_HIGHBITS_STATE_H
#define LATX_GPR_HIGHBITS_STATE_H

#include <stdbool.h>
#include <stdint.h>

#define LATXS_GPR_HB_REG_COUNT 32

typedef enum LATXSGPRHighBitsForm {
    LATXS_GPR_HB_UNKNOWN = 0,
    LATXS_GPR_HB_SIGNED_32,
    LATXS_GPR_HB_ZERO_32,
} LATXSGPRHighBitsForm;

typedef struct LATXSGPRHighBitsState {
    int8_t producer_reg;
    uint8_t producer_form;
} LATXSGPRHighBitsState;

static inline bool latxs_gpr_hb_reg_is_trackable(int reg)
{
    return reg > 0 && reg < LATXS_GPR_HB_REG_COUNT;
}

static inline void latxs_gpr_hb_state_reset(LATXSGPRHighBitsState *state)
{
    state->producer_reg = -1;
    state->producer_form = LATXS_GPR_HB_UNKNOWN;
}

static inline void latxs_gpr_hb_state_clear_producer(
    LATXSGPRHighBitsState *state)
{
    state->producer_reg = -1;
    state->producer_form = LATXS_GPR_HB_UNKNOWN;
}

static inline void latxs_gpr_hb_state_note_producer(
    LATXSGPRHighBitsState *state, int reg, LATXSGPRHighBitsForm form)
{
    if (!latxs_gpr_hb_reg_is_trackable(reg) ||
        form == LATXS_GPR_HB_UNKNOWN) {
        latxs_gpr_hb_state_clear_producer(state);
        return;
    }
    state->producer_reg = reg;
    state->producer_form = form;
}

static inline bool latxs_gpr_hb_state_can_eliminate_extension(
    LATXSGPRHighBitsState *state, int reg, LATXSGPRHighBitsForm form)
{
    if (!latxs_gpr_hb_reg_is_trackable(reg) ||
        form == LATXS_GPR_HB_UNKNOWN) {
        return false;
    }
    if (state->producer_reg != reg ||
        state->producer_form == LATXS_GPR_HB_UNKNOWN) {
        return false;
    }
    return state->producer_form == form;
}

#endif
