/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <sys/mman.h>
#include <unistd.h>
#include <assert.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#include <pthread.h>

#include "qemu/osdep.h"
#include "qemu/compiler.h"
#include "qemu/host-utils.h"
#include "qemu/typedefs.h"
#include "exec/cpu-defs.h"
#include "cpu.h"
#include "exec/cpu-all.h"
#include "exec/exec-all.h"
#include "qemu/xxhash.h"
#include "exec/ram_addr.h"

#include "lsenv.h"

#include "dune-info.h"
#include "dune-kvm-init-value.h"

#include "hamt.h"
#include "hamt-internal.h"
#include "hamt-tlb.h"
#include "hamt-data.h"

#include "loongarch-csr.h"

#include "latx-hamt.h"
#include "latx-options.h"

extern uint64_t hamt_err_data_area[];

void hamt_err_handler(uint64_t estat,
        CPUX86State *env, uint32_t *epc)
{
    env = (void *)DMW_OUT((uint64_t)env);
    epc = (void *)DMW_OUT((uint64_t)epc);

    /*
     * may not come from CodeCache (native context)
     *
     * Only in native context, env is stored in its
     * mapping register, which will be moved into $a2
     * before calling hamt_err_handler
     */
    env = lsenv->cpu_state;

    CPUState *cpu = env_cpu(env);
    int cpuid = cpu->cpu_index;
    uint64_t *errdata = (void *)(hamt_err_data_area[cpuid]);

    //int errcode = errdata[HD_ERRCODE_INDEX];

    latx_dump_hamt_context();

    dune_info("-------- hamt err handler --------------");
    dune_info(" CPU %d", cpuid);
    //dune_info(" errorcode 0x%lx", errcode);
    dune_info(" errordata[1] 0x%lx", errdata[HD_ERRDATA1_INDEX]);
    dune_info(" errordata[2] 0x%lx", errdata[HD_ERRDATA2_INDEX]);
    dune_info(" errordata[3] 0x%lx", errdata[HD_ERRDATA3_INDEX]);
    dune_info(" errordata[4] 0x%lx", errdata[HD_ERRDATA4_INDEX]);
    dune_info(" errordata[5] 0x%lx", errdata[HD_ERRDATA5_INDEX]);
    dune_info(" errordata[6] 0x%lx", errdata[HD_ERRDATA6_INDEX]);
    dune_info(" errordata[7] 0x%lx", errdata[HD_ERRDATA7_INDEX]);
    dune_info(" errordata[8] 0x%lx", errdata[HD_ERRDATA8_INDEX]);
    dune_info("----------------------------------------");
    dune_info(" EPC   %p", (void *)epc);
    dune_info(" BADV  %p", (void *)errdata[PT_BADV_INDEX]);
    dune_info(" ASID  %p", (void *)errdata[PT_ASID_INDEX]);
    dune_info(" ESTAT 0x%lx", estat);
    dune_info("----------------------------------------");
    dune_info(" x64 | i386 | Loong | Value");
    dune_info("-----|------|-------|-------------------");
    dune_info("     |      | 01 RA | 0x%lx", errdata[1]);
    dune_info("     |      | 02 TP | 0x%lx", errdata[2]);
    dune_info("     |      | 03 SP | 0x%lx", errdata[3]);
    dune_info("-----|------|-------|-------------------");
    dune_info("     |      | 04 A0 | 0x%lx", errdata[4]);
    dune_info("     |      | 05 A1 | 0x%lx", errdata[5]);
    dune_info("     |      | 06 A2 | 0x%lx", errdata[6]);
    dune_info("     |      | 07 A3 | 0x%lx", errdata[7]);
    dune_info(" TMP | STMP | 08 A4 | 0x%lx", errdata[8]);
    dune_info(" TMP | STMP | 09 A5 | 0x%lx", errdata[9]);
    dune_info(" TB  | TB   | 10 A6 | 0x%lx", errdata[10]);
    dune_info(" TMP |      | 11 A7 | 0x%lx", errdata[11]);
    dune_info("-----|------|-------|-------------------");
    dune_info(" R8  | TMP  | 12 T0 | 0x%lx", errdata[12]);
    dune_info(" R9  | TMP  | 13 T1 | 0x%lx", errdata[13]);
    dune_info(" R10 | TMP  | 14 T2 | 0x%lx", errdata[14]);
    dune_info(" R11 | TMP  | 15 T3 | 0x%lx", errdata[15]);
    dune_info(" R12 | TMP  | 16 T4 | 0x%lx", errdata[16]);
    dune_info(" R13 | TMP  | 17 T5 | 0x%lx", errdata[17]);
    dune_info(" R14 | TMP  | 18 T6 | 0x%lx", errdata[18]);
    dune_info(" R15 | TMP  | 19 T7 | 0x%lx", errdata[19]);
    dune_info("-----|------|-------|-------------------");
    dune_info(" TMP |      | 20 T8 | 0x%lx", errdata[20]);
    dune_info(" TMP |      | 21 X  | 0x%lx", errdata[21]);
    dune_info(" TMP |      | 22 FP | 0x%lx", errdata[22]);
    dune_info(" ENV | ENV  | 23 S0 | 0x%lx", errdata[23]);
    dune_info("-----|------|-------|-------------------");
    dune_info(" RAX | EAX  | 24 S1 | 0x%lx", errdata[24]);
    dune_info(" RCX | ECX  | 25 S2 | 0x%lx", errdata[25]);
    dune_info(" RDX | EDX  | 26 S3 | 0x%lx", errdata[26]);
    dune_info(" RBX | EBX  | 27 S4 | 0x%lx", errdata[27]);
    dune_info(" RSP | ESP  | 28 S5 | 0x%lx", errdata[28]);
    dune_info(" RBP | EBP  | 29 S6 | 0x%lx", errdata[29]);
    dune_info(" RSI | ESI  | 30 S7 | 0x%lx", errdata[30]);
    dune_info(" RDI | EDI  | 31 S8 | 0x%lx", errdata[31]);
    dune_info("----------------------------------------");

    while (1);
}
