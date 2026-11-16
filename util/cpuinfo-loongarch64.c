/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Host specific cpu identification for LoongArch.
 */

#include "qemu/osdep.h"
#include "host/cpuinfo.h"

#ifdef CONFIG_GETAUXVAL
# include <sys/auxv.h>
#else
# include "elf.h"
#endif
#include <asm/hwcap.h>

#include <larchintrin.h>

unsigned cpuinfo;

/* Called both as constructor and (possibly) via other constructors. */
unsigned __attribute__((constructor)) cpuinfo_init(void)
{
    unsigned info = cpuinfo;
    unsigned cpucfg2 = 0;

    if (info) {
        return info;
    }

    info = CPUINFO_ALWAYS;
    cpucfg2 = __cpucfg(0x2);

    if (cpucfg2 & CPUCFG2_LSX) {
        info |= CPUINFO_LSX;
    }

    if (cpucfg2 & CPUCFG2_LASX) {
        info |= CPUINFO_LASX;
    }

    if (cpucfg2 & CPUCFG2_LAM_BH) {
        info |= CPUINFO_LAM_BH;
    }

    if (cpucfg2 & CPUCFG2_LAMCAS) {
        info |= CPUINFO_LAMCAS;
    }

    if (cpucfg2 & CPUCFG2_SCQ) {
        info |= CPUINFO_SCQ;
    }

    cpuinfo = info;
    return info;
}
