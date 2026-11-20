/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _DUNE_KVM_H_
#define _DUNE_KVM_H_

#include <linux/kvm.h>
#include <pthread.h>
#include <sys/types.h>

struct thread_info {
    struct kvm_regs regs;
    struct kvm_fpu fpu;
    uint64_t era;
    void *ebase;
};

#define KVM_MAX_VCPUS 16

struct dune_kvm_cpu;
struct dune_vcpu_pool_elem {
    bool valid;
    struct dune_kvm_cpu *vcpu;
};

struct dune_kvm_vm {
    int sys_fd;
    int vm_fd;
    int kvm_run_mmap_size;
    int is_fork;

    struct dune_vcpu_pool_elem vcpu_pool[KVM_MAX_VCPUS];
    pthread_spinlock_t lock;
};

struct dune_kvm_cpu {
    int cpu_id;
    int vcpu_id;
    struct dune_kvm_vm *vm;
    int vcpu_fd; /* For VCPU ioctls() */
    struct kvm_run *kvm_run;
    uint64_t syscall_parameter[8];
    int hamt_mode;
    int is_fork;
    int fake_fork;
    pid_t fake_fork_pid;
    void (*fake_fork_fn)(void *);
    void *fake_fork_data;

    struct thread_info info;
};

struct dune_kvm_cpu *dune_init_kvm_cpu(int cpuid, int is_fork);
void dune_enter_kvm(struct dune_kvm_cpu *cpu, int mode, int cpuid);
void dune_kvm_free_vcpu(struct dune_kvm_cpu *vcpu);

void dune_kvm_enable_fpu(struct dune_kvm_cpu *cpu);
int dune_kvm_get_fpu_regs(struct dune_kvm_cpu *cpu,
                    const struct kvm_fpu *fpu_regs);
int dune_kvm_set_fpu_regs(struct dune_kvm_cpu *cpu,
                    const struct kvm_fpu *fpu_regs);

#endif
