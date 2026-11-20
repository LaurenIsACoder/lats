/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#include "target/i386/latx/include/gpr-highbits-policy.h"

static void test_dynamic_tu_is_enabled_with_aot_system(void)
{
    g_assert_true(latxs_gpr_hb_dynamic_tu_is_active(false, true, true));
}

static void test_aot_translation_is_not_dynamic_tu(void)
{
    g_assert_false(latxs_gpr_hb_dynamic_tu_is_active(true, true, true));
}

static void test_disabled_tu_is_rejected(void)
{
    g_assert_false(latxs_gpr_hb_dynamic_tu_is_active(false, false, true));
}

static void test_disabled_option_is_rejected(void)
{
    g_assert_false(latxs_gpr_hb_dynamic_tu_is_active(false, true, false));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/latx/gpr-highbits-policy/dynamic-with-aot-system",
                    test_dynamic_tu_is_enabled_with_aot_system);
    g_test_add_func("/latx/gpr-highbits-policy/aot-translation",
                    test_aot_translation_is_not_dynamic_tu);
    g_test_add_func("/latx/gpr-highbits-policy/tu-disabled",
                    test_disabled_tu_is_rejected);
    g_test_add_func("/latx/gpr-highbits-policy/option-disabled",
                    test_disabled_option_is_rejected);
    return g_test_run();
}
