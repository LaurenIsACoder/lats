/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SYSEMU_LATX_AOT_H
#define SYSEMU_LATX_AOT_H

/* Called with vCPUs stopped, before VM memory is released. */
void aot_shutdown(void);

#endif
