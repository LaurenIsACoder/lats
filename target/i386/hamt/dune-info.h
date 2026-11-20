/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _DUNE_INFO_H_
#define _DUNE_INFO_H_

void dune_warn(const char *err, ...);
void dune_info(const char *info, ...);
void dune_die(const char *err, ...);
void dune_die_exit(const char *err, ...);

#endif
