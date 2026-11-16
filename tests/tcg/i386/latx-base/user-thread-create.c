/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <pthread.h>
#include <stdio.h>
static void *worker(void *p) { return p; }
int main(void) { pthread_t t; int r=pthread_create(&t,0,worker,0); printf("CREATE=%d\n",r); if(r)return 1; r=pthread_join(t,0); printf("JOIN=%d\n",r); return r!=0; }
