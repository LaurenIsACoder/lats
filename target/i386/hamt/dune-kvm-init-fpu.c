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
#include "host/cpuinfo.h"
#include "latx-options.h"

#include "dune-arch.h"
#include "dune-info.h"
#include "dune-kvm.h"
#include "dune-kvm-init.h"
#include "dune-kvm-init-value.h"
#include "loongarch-extcontext.h"
#include "hamt-entry.h"

extern void get_fpu_regs_lasx(struct kvm_fpu *); /* entry-fpu.S */
extern void get_fpu_regs_lsx(struct kvm_fpu *); /* entry-fpu.S */
extern void get_fpu_regs_fp(struct kvm_fpu *); /* entry-fpu.S */

enum ACCESS_OP {
    GET = KVM_GET_ONE_REG,
    SET = KVM_SET_ONE_REG,
};

void dune_kvm_enable_fpu(struct dune_kvm_cpu *cpu)
{
#ifdef LOONGARCH_NEW_WORLD
#ifdef CONFIG_LATX_PRINT
    dune_info("no need to enable FPU cap");
#endif
#else
    struct kvm_enable_cap cap;
    memset(&cap, 0, sizeof(cap));

    cap.cap = KVM_CAP_LOONGARCH_FPU;
    if (ioctl(cpu->vcpu_fd, KVM_ENABLE_CAP, &cap) < 0) {
        dune_die("Fail to enable FPU in guest");
    } else {
#ifdef CONFIG_LATX_PRINT
        dune_info("Success to enable FPU in guest");
#endif
    }

    cap.cap = KVM_CAP_LOONGARCH_LSX;
    if (ioctl(cpu->vcpu_fd, KVM_ENABLE_CAP, &cap) < 0) {
        dune_die("Fail to enable LSX in guest");
    } else {
#ifdef CONFIG_LATX_PRINT
        dune_info("Success to enable LSX in guest");
#endif
    }
#endif
}

static inline int dune_kvm_access_fpu_regs(struct dune_kvm_cpu *vcpu,
                                     const struct kvm_fpu *fpu_regs,
                                           enum ACCESS_OP op)
{
    return ioctl(vcpu->vcpu_fd,
                 op == SET ? KVM_SET_FPU : KVM_GET_FPU,
                 &(vcpu->info.fpu));
}

int dune_kvm_get_fpu_regs(struct dune_kvm_cpu *cpu,
                     const struct kvm_fpu *fpu_regs)
{
    return dune_kvm_access_fpu_regs(cpu, fpu_regs, GET);
}

int dune_kvm_set_fpu_regs(struct dune_kvm_cpu *cpu,
                     const struct kvm_fpu *fpu_regs)
{
    return dune_kvm_access_fpu_regs(cpu, fpu_regs, SET);
}

void dune_kvm_init_fpu(struct dune_kvm_cpu *cpu, int mode, int cpuid)
{
#ifdef CONFIG_LATX_PRINT
    dune_info("--------------------------------");
    dune_info("%s cpu %d mode %d", __func__, cpuid, mode);
#endif

    dune_kvm_enable_fpu(cpu);

    struct kvm_fpu fpu_regs;
    if (option_enable_lasx) {
        get_fpu_regs_lasx(&fpu_regs);
    } else if (cpuinfo & CPUINFO_LSX) {
        get_fpu_regs_lsx(&fpu_regs);
    } else {
        get_fpu_regs_fp(&fpu_regs);
    }

    DUNE_BUILD_ASSERT(offsetof(struct kvm_fpu, fcsr)    == VCPU_FCSR0);
    DUNE_BUILD_ASSERT(offsetof(struct kvm_fpu, fcc)     == VCPU_FCC);
    DUNE_BUILD_ASSERT(offsetof(struct kvm_fpu, fpr[0])  == VCPU_FPR0);
    DUNE_BUILD_ASSERT(offsetof(struct kvm_fpu, fpr[31]) == VCPU_FPR0 + 31 * VCPU_FPR_LEN);

    if (dune_kvm_set_fpu_regs(cpu, &fpu_regs) < 0) {
        dune_die("%s:%d fail to set fpu regs\n", __func__, __LINE__);
    }
}
