/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Host specific cpu identification for LoongArch
 */

#ifndef HOST_CPUINFO_H
#define HOST_CPUINFO_H

#include "qemu/bitops.h"

/* cpuip */
#define CPUCFG2_LSX             BIT(6)
#define CPUCFG2_LASX            BIT(7)
#define CPUCFG2_LAM_BH          BIT(27)
#define CPUCFG2_LAMCAS          BIT(28)
#define CPUCFG2_SCQ             BIT(30)

#define CPUINFO_ALWAYS          BIT(0)  /* so cpuinfo is nonzero */
#define CPUINFO_LSX             BIT(1)
#define CPUINFO_LASX            BIT(2)
#define CPUINFO_LAM_BH          BIT(3)
#define CPUINFO_LAMCAS          BIT(4)
#define CPUINFO_SCQ             BIT(5)

/* Initialized with a constructor. */
extern unsigned cpuinfo;

/*
 * We cannot rely on constructor ordering, so other constructors must
 * use the function interface rather than the variable above.
 */
unsigned cpuinfo_init(void);

#endif /* HOST_CPUINFO_H */
