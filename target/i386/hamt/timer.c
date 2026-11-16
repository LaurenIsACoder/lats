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
#include <larchintrin.h>

#include "qemu/osdep.h"
#include "qemu/compiler.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "qemu/typedefs.h"
#include "exec/cpu-defs.h"
#include "cpu.h"
#include "exec/cpu-all.h"
#include "exec/exec-all.h"
#include "qemu/xxhash.h"

#include "hamt-internal.h"
#include "dune-kvm-init-value.h"
#include "htimer.h"
#include "lsenv.h"
#include "loongarch-csr.h"
#include "latx-options.h"
#include "latx-ras.h"
#include "hamt-data.h"
#include "hamt-init.h"
#include "hw/i386/apic.h"

#define TYPE_APIC "apic"
/*This is reusing the APICCommonState typedef from APIC_COMMON */
DECLARE_INSTANCE_CHECKER(APICCommonState, APIC,
                         TYPE_APIC)

#define NSEC_PER_SEC                1000000000L
#define MNSEC_PER_SEC               (NSEC_PER_SEC >> 20)
#define LOONGARCH_CPUCFG2           0x2
#define CPUCFG2_LLFTP               BIT(14)
#define LOONGARCH_CPUCFG4           0x4
#define LOONGARCH_CPUCFG5           0x5

static uint64_t timer_mhz;

static inline unsigned int calc_const_freq(void)
{
        unsigned int res;
        unsigned int base_freq;
        unsigned int cfm, cfd;

        res = __cpucfg(LOONGARCH_CPUCFG2);
        if (!(res & CPUCFG2_LLFTP))
                return 0;

        base_freq = __cpucfg(LOONGARCH_CPUCFG4);
        res = __cpucfg(LOONGARCH_CPUCFG5);
        cfm = res & 0xffff;
        cfd = (res >> 16) & 0xffff;

        if (!base_freq || !cfm || !cfd)
                return 0;

        return (base_freq * cfm / cfd);
}

void init_timer_mhz(void)
{
    unsigned long hz;
    // 100000000
    hz = calc_const_freq();
    // 95
    timer_mhz = hz >> 20;
}

static uint32_t convert_Timertick_to_x86Count(APICCommonState *s)
{
    int32_t x86_count;
    uint64_t timertick = read_csr_timertick();

    x86_count = (timertick * MNSEC_PER_SEC / timer_mhz) >> s->count_shift;
    return x86_count;
}

uint32_t get_current_count_from_timertick(APICCommonState *s)
{
    uint32_t x86_count = convert_Timertick_to_x86Count(s);
    HTIMER_LOG("[cpu%d] %s x86_count 0x%x\n",
                CPU(s->cpu)->cpu_index, __func__, x86_count);
    return x86_count;
}

static uint64_t convert_x86InitialCount_to_TcfgVal(APICCommonState *s)
{
    uint64_t tval;
    int64_t detla;

    detla = ((uint64_t)s->initial_count + 1) << s->count_shift;
    tval = detla * timer_mhz / MNSEC_PER_SEC;
    return tval;
}

static void update_timer_ecfg(APICCommonState *s)
{
    uint64_t ecfg = s->la_ecfg;
    if (s->lvt[APIC_LVT_TIMER] & APIC_LVT_MASKED) {
        if (ecfg & ECFGF_TIMER) {
            ecfg &= ~ECFGF_TIMER;
            write_csr_ecfg(ecfg);
            s->la_ecfg = ecfg;
        }
    } else {
        if (!(ecfg & ECFGF_TIMER)) {
            ecfg |= ECFGF_TIMER;
            write_csr_ecfg(ecfg);
            s->la_ecfg = ecfg;
        }
    }
}

void update_tcfg(APICCommonState *s)
{
    uint64_t timer_config = 0;
    uint32_t lvtt = s->lvt[APIC_LVT_TIMER];

    if (s->initial_count) {
        update_timer_ecfg(s);
        timer_config = convert_x86InitialCount_to_TcfgVal(s) | CSR_TCFG_EN;
        if (lvtt & APIC_LVT_TIMER_PERIODIC) {
            timer_config |= CSR_TCFG_PERIOD;
        }
    }

    HTIMER_LOG("[cpu%d] %s 0x%lx => 0x%lx (periodic 0x%x masked 0x%x "
            "level-trigger %d remote-irr 0x%x int-polarity 0x%x "
            "deliv-sts 0x%x deliv-mod 0x%x)\n", CPU(s->cpu)->cpu_index,
            __func__, read_csr_timercfg(), timer_config,
            (lvtt & APIC_LVT_TIMER_PERIODIC) >> APIC_LVT_TIMER_SHIFT,
            (lvtt & APIC_LVT_MASKED) >> APIC_LVT_MASKED_SHIFT,
            (lvtt & APIC_LVT_LEVEL_TRIGGER) >> APIC_LVT_LEVEL_TRIGGER_SHIFT,
            (lvtt & APIC_LVT_REMOTE_IRR) >> APIC_LVT_REMOTE_IRR_SHIFT,
            (lvtt & APIC_LVT_INT_POLARITY) >> APIC_LVT_INT_POLARITY_SHIFT,
            (lvtt & APIC_LVT_DELIV_STS) >> APIC_LVT_DELIV_STS_SHIFT,
            (lvtt & APIC_LVT_DELIV_MOD) >> APIC_LVT_DELIV_MOD_SHIFT);

    write_csr_timercfg(timer_config);
}

void apic_timer_deliver_from_loongarch(CPUX86State *env)
{
    if (!option_htimer) {
        return;
    }

    /* This entry receives the restored real TP, so TLS is safe here. */
    if (option_rastoptp) {
        env = lsenv->cpu_state;
    }
    apic_local_deliver_timer();
}
