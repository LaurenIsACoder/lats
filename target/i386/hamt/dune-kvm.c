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

#include "dune-arch.h"
#include "dune-info.h"
#include "dune-stack.h"
#include "dune-kvm.h"
#include "dune-kvm-init.h"
#include "loongarch-extcontext.h"

#include "cpu.h"
#include "hamt-options.h"
#include "hamt-va.h"
#include "latx-hamt.h"
#include "latx-options.h"

void dune_kvm_free_vcpu(struct dune_kvm_cpu *vcpu)
{
    struct dune_kvm_vm *kvm = vcpu->vm;

    if (pthread_spin_lock(&kvm->lock)) {
        dune_die("locked failed");
    }
    assert(kvm->vcpu_pool[vcpu->cpu_id].valid);
    kvm->vcpu_pool[vcpu->cpu_id].valid = false;

    if (pthread_spin_unlock(&kvm->lock)) {
        dune_die("unlocked failed");
    }
}

static struct dune_kvm_cpu fork_kvm_vcpu;

static struct dune_kvm_cpu *dune_kvm_alloc_vcpu(struct dune_kvm_vm *vm,
        int vcpuid)
{
    struct dune_kvm_cpu *vcpu;
    int cpu_id = -1;

    if (vm->is_fork) {
        vcpu = &fork_kvm_vcpu;
        memset(vcpu, 0, sizeof(struct dune_kvm_cpu));
        vcpu->is_fork = 1;
        cpu_id = 0;
        goto after_alloc_vcpu;
    }

    pthread_spin_lock(&vm->lock);

    for (int i = 0; i < KVM_MAX_VCPUS; ++i) {
        if (vm->vcpu_pool[i].valid) {
            continue;
        }

        vm->vcpu_pool[i].valid = true;
        if (vm->vcpu_pool[i].vcpu != NULL) {
            if (pthread_spin_unlock(&vm->lock)) {
                if (vm->is_fork) dune_die_exit("unlocked failed");
                dune_die("unlocked failed");
            }
            return vm->vcpu_pool[i].vcpu;
        } else {
            cpu_id = i;
            break;
        }
    }

    pthread_spin_unlock(&vm->lock);

    if (cpu_id == -1) {
        if (vm->is_fork) dune_die_exit("No more vcpu to allocate\n");
        dune_die("No more vcpu to allocate\n");
    }

    vcpu = calloc(1, sizeof(struct dune_kvm_cpu));
    if (!vcpu) {
        return NULL;
    }

after_alloc_vcpu:

    vm->vcpu_pool[cpu_id].vcpu = vcpu;
    vcpu->vm = vm;
    vcpu->cpu_id = cpu_id; // internal id
    vcpu->vcpu_id = vcpuid; // equal to current_cpu->cpu_index

    vcpu->vcpu_fd = ioctl(vcpu->vm->vm_fd, KVM_CREATE_VCPU, vcpu->vcpu_id);
    if (vcpu->vcpu_fd < 0) {
        if (vm->is_fork) dune_die_exit("KVM_CREATE_VCPU ioctl");
        dune_die("KVM_CREATE_VCPU ioctl");
    } else {
#ifdef CONFIG_LATX_PRINT
        dune_info("KVM_CREATE_VCPU[%d] CPUID %d", cpu_id, vcpuid);
#endif
    }

    vcpu->kvm_run = mmap(NULL, vm->kvm_run_mmap_size,
            PROT_READ | PROT_WRITE, MAP_SHARED,
            vcpu->vcpu_fd, 0);
    if (vcpu->kvm_run == MAP_FAILED) {
        if (vm->is_fork) dune_die_exit("unable to mmap vcpu fd");
        dune_die("unable to mmap vcpu fd");
    }

    return vcpu;
}

extern struct dune_kvm_vm *vm;

static struct dune_kvm_vm hamt_kvm;
static struct dune_kvm_vm fork_kvm;

struct dune_kvm_cpu *dune_init_kvm_cpu(int cpuid, int is_fork)
{
    char dev_path[] = "/dev/kvm";
    int ret;

    if (vm != NULL) {
        return dune_kvm_alloc_vcpu(vm, cpuid);
    }

    if (is_fork) {
        vm = &fork_kvm;
        memset(vm, 0, sizeof(struct dune_kvm_vm));
    } else {
        vm = &hamt_kvm;
        memset(vm, 0, sizeof(struct dune_kvm_vm));
    }

    vm->is_fork = is_fork;
    vm->sys_fd = -1;
    vm->vm_fd = -1;
    vm->kvm_run_mmap_size = -1;

    if (pthread_spin_init(&vm->lock, PTHREAD_PROCESS_PRIVATE) != 0) {
        if (is_fork) dune_die_exit("pthread_spin_init failed\n");
        dune_die("pthread_spin_init failed\n");
    }

    for (int i = 0; i < KVM_MAX_VCPUS; ++i) {
        vm->vcpu_pool[i].valid = false;
        vm->vcpu_pool[i].vcpu = NULL;
    }

    ret = open(dev_path, O_RDWR);
    if (ret < 0) {
        if (is_fork) dune_die_exit("unable to open %s", dev_path);
        dune_die("unable to open %s", dev_path);
    } else {
        vm->sys_fd = ret;
    }

    ret = ioctl(vm->sys_fd, KVM_GET_API_VERSION, 0);
    if (ret != KVM_API_VERSION) {
        if (is_fork) dune_die_exit("KVM_GET_API_VERSION");
        dune_die("KVM_GET_API_VERSION");
    }

    /*
     * 在调用路径中 kvm_create_vm => kvm_init_mmu_notifier =>
     * do_mmu_notifier_register => mm_take_all_locks => signal_pending
     * 如果 parent 正好发送一个信号，那么 KVM_CREATE_VM 会失败
     */
    while (true) {
        ret = ioctl(vm->sys_fd, KVM_CREATE_VM, 0);
        if (ret < 0) {
            if (errno != EINTR) {
                if (is_fork) dune_die_exit("KVM_CREATE_VM");
                dune_die("KVM_CREATE_VM");
            } else {
                continue;
            }
        }
        vm->vm_fd = ret;
#ifdef CONFIG_LATX_PRINT
        dune_info("KVM_CREATE_VM");
#endif
        break;
    };

    int mmap_size = ioctl(vm->sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < 0) {
        if (is_fork) dune_die_exit("KVM_GET_VCPU_MMAP_SIZE");
        dune_die("KVM_GET_VCPU_MMAP_SIZE");
    }
    vm->kvm_run_mmap_size = mmap_size;

    uint64_t kvm_gpa_base  = 0x0;
    uint64_t kvm_user_base = 0x0;
    uint64_t kvm_mem_size  = 1ULL << 40;
    if (is_host_kernel_48_va()) {
        kvm_gpa_base  = 0x7fULL << 40;
        kvm_user_base = 0x7fULL << 40;
        /*kvm_mem_size -= 1ULL << 14;*/
    }

    struct kvm_userspace_memory_region mem =
        (struct kvm_userspace_memory_region){
            .slot = 0,
            .flags = 0,
            .guest_phys_addr = kvm_gpa_base,
            .userspace_addr  = kvm_user_base,
            .memory_size = kvm_mem_size,
        };

    ret = ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem);
    if (ret < 0) {
        if (is_fork) dune_die_exit("KVM_SET_USER_MEMORY_REGION");
        dune_die("KVM_SET_USER_MEMORY_REGION");
    } else {
        if (is_host_kernel_48_va()) {
            dune_info("KVM_SET_USER_MEMORY_REGION 0x7f00_0000_0000");
        } else {
            dune_info("KVM_SET_USER_MEMORY_REGION 0x0000_0000_0000");
        }
    }

#ifdef LOONGARCH_NEW_WORLD
    if (is_host_kernel_48_va()) {
        mem = (struct kvm_userspace_memory_region) {
            .slot = 1, .flags = 0,
            .guest_phys_addr = 0x555000000000,
            .memory_size = 1ULL << 36,
            .userspace_addr = 0x555000000000,
        };
        ret = ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem);
        if (ret < 0) {
            if (is_fork) dune_die_exit("KVM_SET_USER_MEMORY_REGION 0x5550_0000_0000");
            dune_die("KVM_SET_USER_MEMORY_REGION 0x5550_0000_0000");
        } else {
            dune_info("KVM_SET_USER_MEMORY_REGION 0x5550_0000_0000");
        }

        // sptb store TB at 0x9b_0000_0000
        // TB address = 0x9b00000000 | TB's low 32-bit
        if (option_split_tb) {
            mem = (struct kvm_userspace_memory_region) {
                .slot = 2, .flags = 0,
                .guest_phys_addr = 0x9b00000000,
                .memory_size = 1ULL << 32,
                .userspace_addr = 0x9b00000000,
            };
            ret = ioctl(vm->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem);
            if (ret < 0) {
                if (is_fork) dune_die_exit("KVM_SET_USER_MEMORY_REGION 0x009b_0000_0000");
                dune_die("KVM_SET_USER_MEMORY_REGION 0x009b_0000_0000");
            } else {
                dune_info("KVM_SET_USER_MEMORY_REGION 0x009b_0000_0000");
            }
        }
    }

    // create ipi device
    struct kvm_create_device kcd;
    kcd.flags = 0;
    kcd.type = 12; // KVM_DEV_TYPE_LOONGARCH_IPI
    ret = ioctl(vm->vm_fd, KVM_CREATE_DEVICE, &kcd);
    if (ret < 0) {
        if (is_fork) dune_die_exit("KVM_CREATE_DEVICE 12 IPI");
        dune_die("KVM_CREATE_DEVICE 12 IPI");
    } else {
        dune_info("KVM_CREATE_DEVICE 12 IPI");
    }
#else

    ret = ioctl(vm->vm_fd, KVM_CREATE_IRQCHIP);
    if (ret < 0) {
        if (is_fork) dune_die_exit("KVM_CREATE_IRQCHIP");
        dune_die("KVM_CREATE_IRQCHIP");
    }
#endif

    return dune_kvm_alloc_vcpu(vm, cpuid);
}

#ifdef CONFIG_LATX_PRINT
static void dune_kvm_dump_regs(struct kvm_regs *regs)
{
    dune_info("---------------------");
    dune_info("Initial KVM Registers");

#define DUNE_DUMP_KVM_REGS(str, index) do {                 \
    dune_info("$"str" : %016llx %016llx %016llx %016llx",   \
        (unsigned long long)regs->gpr[index + 0],           \
        (unsigned long long)regs->gpr[index + 1],           \
        (unsigned long long)regs->gpr[index + 2],           \
        (unsigned long long)regs->gpr[index + 3]);          \
} while (0)
    DUNE_DUMP_KVM_REGS("0 ", 0);
    DUNE_DUMP_KVM_REGS("4 ", 4);
    DUNE_DUMP_KVM_REGS("8 ", 8);
    DUNE_DUMP_KVM_REGS("12", 12);
    DUNE_DUMP_KVM_REGS("16", 16);
    DUNE_DUMP_KVM_REGS("20", 20);
    DUNE_DUMP_KVM_REGS("24", 24);
    DUNE_DUMP_KVM_REGS("28", 28);

    dune_info("pc  : %016llx", (unsigned long long)regs->pc);
}
#endif

static int __attribute__((noinline))
dune_launch_kvm(struct dune_kvm_cpu *cpu,
        struct kvm_regs *regs, int mode, int cpuid)
{
    asm goto("\n\t"
         "st.d $r0,  $r5, 0\n\t"
         "st.d $r1,  $r5, 8\n\t"
         "st.d $r2,  $r5, 16\n\t"
         "st.d $r3,  $r5, 24\n\t"
         "st.d $r4,  $r5, 32\n\t"
         "st.d $r5,  $r5, 40\n\t"
         "st.d $r6,  $r5, 48\n\t"
         "st.d $r7,  $r5, 56\n\t"
         "st.d $r8,  $r5, 64\n\t"
         "st.d $r9,  $r5, 72\n\t"
         "st.d $r10, $r5, 80\n\t"
         "st.d $r11, $r5, 88\n\t"
         "st.d $r12, $r5, 96\n\t"
         "st.d $r13, $r5, 104\n\t"
         "st.d $r14, $r5, 112\n\t"
         "st.d $r15, $r5, 120\n\t"
         "st.d $r16, $r5, 128\n\t"
         "st.d $r17, $r5, 136\n\t"
         "st.d $r18, $r5, 144\n\t"
         "st.d $r19, $r5, 152\n\t"
         "st.d $r20, $r5, 160\n\t"
         "st.d $r21, $r5, 168\n\t"
         "st.d $r22, $r5, 176\n\t"
         "st.d $r23, $r5, 184\n\t"
         "st.d $r24, $r5, 192\n\t"
         "st.d $r25, $r5, 200\n\t"
         "st.d $r26, $r5, 208\n\t"
         "st.d $r27, $r5, 216\n\t"
         "st.d $r28, $r5, 224\n\t"
         "st.d $r29, $r5, 232\n\t"
         "st.d $r30, $r5, 240\n\t"
         "st.d $r31, $r5, 248\n\t"

         "la.local $r6, %l[guest_entry]\n\t"
         "st.d $r6, $r5, 256\n\t"
         "ld.d $r6, $r5, 48\n\t" // restore $6
         :
         :
         : "memory"
         : guest_entry);

#ifdef CONFIG_LATX_PRINT
    dune_kvm_dump_regs(regs);
#endif

    dune_kvm_init_ebase(cpu, mode, cpuid);
    dune_kvm_init_csr(cpu, mode, cpuid);
    dune_kvm_init_fpu(cpu, mode, cpuid);

    if (ioctl(cpu->vcpu_fd, KVM_SET_REGS, regs) < 0) {
        dune_die("KVM_SET_REGS failed");
    }

    /*
     * host -> host_loop() -> ioctl(kvm)
     * => guest -> guest_entry -> return
     */
    switch_host(cpu);

    dune_die("host should never reach here\n");

guest_entry:
    dune_info("cpu %d enter guest(HAMT is enabled)", cpuid);
    return 0;
}

void dune_enter_kvm(struct dune_kvm_cpu *cpu, int mode, int cpuid)
{
#ifdef CONFIG_LATX_PRINT
    dune_info("%s cpu %d mode %d", __func__, cpuid, mode);
#endif
    struct kvm_regs regs;
    DUNE_BUILD_ASSERT(256 == offsetof(struct kvm_regs, pc));
    dune_launch_kvm(cpu, &regs, mode, cpuid);
}

uint64_t hamt_kvm_cpu_read_pc(void *cpu)
{
    struct kvm_regs regs;
    struct dune_kvm_cpu *vcpu;

    CPUState *cs = cpu;
    vcpu = cs->hamt_kvmcpu_ptr;

    if (ioctl(vcpu->vcpu_fd, KVM_GET_REGS, &regs)) {
        fprintf(stderr, "%s kvm ioctl error\n", __func__);
        return 0;
    }
    return regs.pc;
}
