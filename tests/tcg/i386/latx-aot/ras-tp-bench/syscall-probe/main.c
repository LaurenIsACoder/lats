/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <sys/mount.h>
#include <unistd.h>

extern uint64_t ras_syscall_probe(void);
unsigned char ras_probe_enabled;

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getpid() == 1) mount("proc", "/proc", "proc", 0, NULL);
    puts("BENCH_READY");
    if (getchar() != 'G') return 1;
    for (int i = 0; i < 10000; i++) {
        if (ras_syscall_probe() != 570) return 2;
    }
    ras_probe_enabled = 1;
    if (ras_syscall_probe() != 570) return 3;
    puts("RAS_PASS 570");
    for (;;) pause();
}
