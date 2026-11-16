/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdint.h>

#include <sys/time.h>
#include <sys/resource.h>

#include "latx-perfmap.h"
#include "latx-hamt.h"
#include "latx-options.h"

#include "qemu/osdep.h"

#include "dune-mmap.h"
#include "dune-arch.h"
#include "dune-info.h"
#include "dune-stack.h"
#include "dune-kvm.h"
#include "dune-kvm-init.h"
#include "dune-kvm-init-value.h"

#include "hamt-enter.h"
#include "hamt-interrupt.h"
#include "hamt-entry-func.h"
#include "hamt-exccode.h"

static void dune_kvm_cpu_set_ebase(struct dune_kvm_cpu *cpu,
        int cpuid, int code, const char *name,
        void *st, void *ed)
{
    assert((ed - st) < 512);
    int offset = VEC_SIZE * code;
    memcpy(cpu->info.ebase + offset, st, ed - st);
    latx_perfmap_insert(cpu->info.ebase + offset, ed - st, name);
#ifdef CONFIG_LATX_PRINT
    dune_info("cpu %d %-32s: from 0x%llx to 0x%llx",
            cpuid, name, st, cpu->info.ebase + offset);
#endif
}

void dune_kvm_init_ebase(struct dune_kvm_cpu *cpu, int mode, int cpuid)
{
    DUNE_BUILD_ASSERT(512 == VEC_SIZE);
    DUNE_BUILD_ASSERT(INT_OFFSET * VEC_SIZE == PAGESIZE * 2);
    DUNE_BUILD_ASSERT(VEC_SIZE * 14 < PAGESIZE);

    uint64_t e = (uint64_t)mmap_pages(8);
    int b14b15 = (e >> 14) & 0x3;
    if (b14b15 == 0x1) e += (0x3<<14);
    if (b14b15 == 0x2) e += (0x2<<14);
    if (b14b15 == 0x3) e += (0x1<<14);
    assert(!(e & 0xffff)); // 64K align

    cpu->info.ebase = (void *)e;
#ifdef CONFIG_LATX_PRINT
    dune_info("--------------------------------");
    dune_info("%s cpu %d mode %d ebase 0x%llx",
            __func__, cpuid, mode, cpu->info.ebase);
#endif
#define INVALID_EBASE_POSITION 0x13
    memset((void *)e, 0, PAGESIZE * 4);

    if (mode == HAMT_MODE_RR) {
        assert(cpuid == 0);
    }
    if (mode == HAMT_MODE_MT) {
        assert(0 <= cpuid && cpuid < HAMT_CPU_MAX);
    }

    char name[64];

    sprintf(name, "hamt_tlb_refill_entry_s%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, 0, name,
            (void *)tlb_refill_entry_begin[cpuid],
            (void *)tlb_refill_entry_end[cpuid]);

    sprintf(name, "hamt_tlb_load_entry_s%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_TLBL, name,
            (void *)tlb_ldstmy_entry_begin[cpuid],
            (void *)tlb_ldstmy_entry_end[cpuid]);

    sprintf(name, "hamt_tlb_store_entry_s%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_TLBS, name,
            (void *)tlb_ldstmy_entry_begin[cpuid],
            (void *)tlb_ldstmy_entry_end[cpuid]);

    sprintf(name, "hamt_tlb_modify_entry_s%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_TLBM, name,
            (void *)tlb_ldstmy_entry_begin[cpuid],
            (void *)tlb_ldstmy_entry_end[cpuid]);

    sprintf(name, "hamt_tlb_read_inhibit_entry_s%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_TLBRI, name,
            (void *)error_tlb_entry_begin[cpuid],
            (void *)error_tlb_entry_end[cpuid]);

    sprintf(name, "hamt_privilege_err_entry_s%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_TLBPE, name,
            (void *)error_tlb_entry_begin[cpuid],
            (void *)error_tlb_entry_end[cpuid]);

    sprintf(name, "hamt_address_error_entry_s%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_ADE, name,
            (void *)error_tlb_entry_begin[cpuid],
            (void *)error_tlb_entry_end[cpuid]);

    sprintf(name, "hamt_inst_fetch_entry_s%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_TLBI, name,
            (void *)error_stop_entry_begin[cpuid],
            (void *)error_stop_entry_end[cpuid]);

    sprintf(name, "hamt_exec_inhibit_entry_s%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_TLBXI, name,
            (void *)error_stop_entry_begin[cpuid],
            (void *)error_stop_entry_end[cpuid]);

    // EXCCODE_ALE 9
    sprintf(name, "hamt_unaligned_access_entry_%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_ALE, name,
            (void *)ale_entry_begin[cpuid],
            (void *)ale_entry_end[cpuid]);


    sprintf(name, "hamt_invalid_inst_entry_s%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_INE, name,
            (void *)ine_entry_begin[cpuid],
            (void *)ine_entry_end[cpuid]);

    if (option_htimer) {
        sprintf(name, "hamt_interrupt_ti%d", cpuid);
        dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_INT_TI, name,
                (void *)hamt_int_ti_entry_begin[cpuid],
                (void *)hamt_int_ti_entry_end[cpuid]);
    }

    if (hamt_enable_ipi()) {
        sprintf(name, "hamt_interrupt_ipi_s%d", cpuid);
        dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_INT_IPI, name,
                (void *)hamt_int_ipi_entry_begin[cpuid],
                (void *)hamt_int_ipi_entry_end[cpuid]);
    }

    // EXCCODE_BP 12 
    sprintf(name, "hamt_break_entry_%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_BP, name,
            (void *)break_entry_begin[cpuid],
            (void *)break_entry_end[cpuid]);

    // EXCCODE_SYS 11
    sprintf(name, "hamt_syscall_entry_%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_SYS, name,
            (void *)syscall_entry_begin,
            (void *)syscall_entry_end);

    // EXCCODE_FPE 18
    sprintf(name, "hamt_fpe_entry_%d", cpuid);
    dune_kvm_cpu_set_ebase(cpu, cpuid, EXCCODE_FPE, name,
            (void *)fpe_entry_begin[cpuid],
            (void *)fpe_entry_end[cpuid]);

    latx_perfmap_flush();
}
