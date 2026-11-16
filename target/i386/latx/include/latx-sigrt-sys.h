/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SIGRT_SYS_H_
#define _LATX_SIGRT_SYS_H_
void lats_init_rr_thread_sigrtmin(CPUState *cpu);
void lats_request_tb_exit(CPUState *cpu);
void lats_notify_after_iothread_unlock(void);
//#define SIGRTMIN_DEBUG
#ifdef SIGRTMIN_DEBUG
#define SIGRTMIN_LOG(format, ...) \
    fprintf(stderr, "[LATS-SIGRTMIN]: " format, ##__VA_ARGS__)
#else
#define SIGRTMIN_LOG(format, ...) ((void)0)
#endif
#endif
