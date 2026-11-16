/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#include "target/i386/latx/include/gpr-highbits-state.h"

static void test_matching_adjacent_extension(void)
{
    LATXSGPRHighBitsState state;

    latxs_gpr_hb_state_reset(&state);
    latxs_gpr_hb_state_note_producer(&state, 5, LATXS_GPR_HB_ZERO_32);

    g_assert_cmpint(state.producer_reg, ==, 5);
    g_assert_cmpint(state.producer_form, ==, LATXS_GPR_HB_ZERO_32);
    g_assert_true(latxs_gpr_hb_state_can_eliminate_extension(
        &state, 5, LATXS_GPR_HB_ZERO_32));
}

static void test_mismatched_form_is_rejected(void)
{
    LATXSGPRHighBitsState state;

    latxs_gpr_hb_state_reset(&state);
    latxs_gpr_hb_state_note_producer(&state, 7, LATXS_GPR_HB_SIGNED_32);

    g_assert_false(latxs_gpr_hb_state_can_eliminate_extension(
        &state, 7, LATXS_GPR_HB_ZERO_32));
}

static void test_different_register_is_rejected(void)
{
    LATXSGPRHighBitsState state;

    latxs_gpr_hb_state_reset(&state);
    latxs_gpr_hb_state_note_producer(&state, 3, LATXS_GPR_HB_ZERO_32);

    g_assert_false(latxs_gpr_hb_state_can_eliminate_extension(
        &state, 4, LATXS_GPR_HB_ZERO_32));
}

static void test_intervening_instruction_clears_proof(void)
{
    LATXSGPRHighBitsState state;

    latxs_gpr_hb_state_reset(&state);
    latxs_gpr_hb_state_note_producer(&state, 9, LATXS_GPR_HB_ZERO_32);
    latxs_gpr_hb_state_clear_producer(&state);

    g_assert_false(latxs_gpr_hb_state_can_eliminate_extension(
        &state, 9, LATXS_GPR_HB_ZERO_32));
    g_assert_cmpint(state.producer_reg, ==, -1);
}

static void test_snapshot_restore(void)
{
    LATXSGPRHighBitsState state;
    LATXSGPRHighBitsState snapshot;

    latxs_gpr_hb_state_reset(&state);
    latxs_gpr_hb_state_note_producer(&state, 11, LATXS_GPR_HB_SIGNED_32);
    snapshot = state;

    latxs_gpr_hb_state_clear_producer(&state);
    state = snapshot;

    g_assert_true(latxs_gpr_hb_state_can_eliminate_extension(
        &state, 11, LATXS_GPR_HB_SIGNED_32));
}

static void test_invalid_producer_clears_slot(void)
{
    LATXSGPRHighBitsState state;

    latxs_gpr_hb_state_reset(&state);
    latxs_gpr_hb_state_note_producer(&state, 6, LATXS_GPR_HB_ZERO_32);
    latxs_gpr_hb_state_note_producer(&state, 0, LATXS_GPR_HB_ZERO_32);

    g_assert_cmpint(state.producer_reg, ==, -1);
    g_assert_false(latxs_gpr_hb_state_can_eliminate_extension(
        &state, 6, LATXS_GPR_HB_ZERO_32));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/latx/gpr-highbits/adjacent-match",
                    test_matching_adjacent_extension);
    g_test_add_func("/latx/gpr-highbits/form-mismatch",
                    test_mismatched_form_is_rejected);
    g_test_add_func("/latx/gpr-highbits/register-mismatch",
                    test_different_register_is_rejected);
    g_test_add_func("/latx/gpr-highbits/intervening-instruction",
                    test_intervening_instruction_clears_proof);
    g_test_add_func("/latx/gpr-highbits/snapshot-restore",
                    test_snapshot_restore);
    g_test_add_func("/latx/gpr-highbits/invalid-producer",
                    test_invalid_producer_clears_slot);
    return g_test_run();
}
