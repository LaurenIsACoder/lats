/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "hamt-entry.h"

.global dune_loop
.global switch_stack
switch_stack:
    move sp, a1
    b dune_loop /* host_loop 绝对不会返回 */
