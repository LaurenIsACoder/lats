/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HAMT_FAKE_FORK_H_
#define _HAMT_FAKE_FORK_H_

#include <sys/types.h>

// @dunekvmcpu : CPUX86State.hamt_dune_kvm_cpu
// @fn : function to execute in child
// @data : parameter for function
// after executing fn(data), child will exit(0)
pid_t hamt_fake_fork(void *dunekvmcpu, void *fn, void *data);

#endif
