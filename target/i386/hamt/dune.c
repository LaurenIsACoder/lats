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

#include "qemu/osdep.h"
#include "exec/cpu-defs.h"

#include "dune-arch.h"
#include "dune-kvm.h"
#include "dune-stack.h"
#include "dune-info.h"

#include "hamt-init.h"
#include "hamt-enter.h"
#include "hamt-internal.h"
#include "hamt-iocsr.h"
#include "hamt-interrupt.h"

struct dune_kvm_vm *vm;

int dune_enter(void *cpu, int mode, int cpuid)
{
#ifdef CONFIG_LATX_PRINT
    dune_info("%s cpu %d mode %d", __func__, cpuid, mode);
#endif

    CPUState *cs = cpu;

    dune_expand_stack();
    struct dune_kvm_cpu *kvmcpu = dune_init_kvm_cpu(cpuid, 0);
    if (kvmcpu == NULL) {
        dune_die("dune_init_kvm_cpu");
    }
    kvmcpu->hamt_mode = mode;

    cs->hamt_kvmcpu_ptr = kvmcpu;

#ifdef CONFIG_LATX_PRINT
    dune_info("init kvm vcpu KVM_vCPU    = %p", kvmcpu);
    dune_info("init kvm vcpu KVM_vCPU_fd = %d", kvmcpu->vcpu_fd);
#endif

    hamt_init_kvm_cpu(cpu, kvmcpu);
    hamt_init_kvm_regs(cpu, sizeof(struct kvm_regs));
    hamt_prepare(mode, cpuid);

    dune_enter_kvm(kvmcpu, kvmcpu->hamt_mode, cpuid);

    hamt_enable_local_ipi();

#ifdef CONFIG_LATX_PRINT
    uint64_t crmd = read_csr_crmd();
    uint64_t ecfg = read_csr_ecfg();
    printf("%s:%d CRMD 0x%lx IE %d\n", __func__, __LINE__, crmd, (int)(crmd>>2)&1);
    printf("%s:%d ECFG 0x%lx IPI %d\n", __func__, __LINE__, ecfg, (int)(ecfg>>12)&1);
#endif

    return 0;
}
