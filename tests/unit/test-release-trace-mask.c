/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * Standalone regression for the release trace mask, without a QEMU runtime.
 * cc -O2 -Wall -Wextra -Werror -DNDEBUG -Iinclude \
 *    tests/unit/test-release-trace-mask.c -o /tmp/test-release-trace-mask
 * Repeat without -DNDEBUG to cover the debug build's unchanged behavior.
 */
#include <stdbool.h>
#include <stdio.h>

/* log-for-trace.h normally obtains this annotation through qemu/osdep.h. */
#define G_GNUC_PRINTF(fmt, args)
#include "qemu/log-for-trace.h"

int qemu_loglevel;
static volatile int debug_argument_evaluations;

static int expensive_debug_argument(void)
{
    return ++debug_argument_evaluations;
}

static bool check(bool condition, const char *description)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description);
    }
    return condition;
}

int main(void)
{
    const int debug_mask = 1 << 9;
    bool ok = true;

    qemu_loglevel = 0;
    ok &= check(!qemu_loglevel_mask(LOG_TRACE), "trace disabled");
    qemu_loglevel = LOG_TRACE;
    ok &= check(qemu_loglevel_mask(LOG_TRACE), "explicit trace enabled");
    ok &= check(!qemu_loglevel_mask(debug_mask), "unselected debug mask");
    qemu_loglevel = debug_mask;
    ok &= check(!qemu_loglevel_mask(LOG_TRACE), "debug mask is not trace");
#ifdef NDEBUG
    ok &= check(!qemu_loglevel_mask(debug_mask), "release debug disabled");
    ok &= check(!qemu_loglevel_mask(debug_mask | LOG_TRACE),
                "mixed mask must not re-enable debug in release");
#else
    ok &= check(qemu_loglevel_mask(debug_mask), "debug logging unchanged");
#endif
    qemu_loglevel = debug_mask | LOG_TRACE;
    if (qemu_loglevel_mask(debug_mask)) {
        (void)expensive_debug_argument();
    }
#ifdef NDEBUG
    ok &= check(debug_argument_evaluations == 0,
                "release must not evaluate expensive debug arguments");
#else
    ok &= check(debug_argument_evaluations == 1,
                "debug still evaluates selected debug arguments");
#endif
    ok &= check(qemu_loglevel_mask(LOG_TRACE), "trace with other bits enabled");
    ok &= check(!qemu_loglevel_mask(0), "zero mask");
    if (ok) {
        puts("PASS: trace selection and debug argument gating");
    }
    return ok ? 0 : 1;
}
