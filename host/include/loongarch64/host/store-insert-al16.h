/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Atomic store insert into 128-bit, AArch64 version.
 *
 * Copyright (C) 2023 Linaro, Ltd.
 */

#ifndef LOONGARCH_STORE_INSERT_AL16_H
#define LOONGARCH_STORE_INSERT_AL16_H

/**
 * store_atom_insert_al16:
 * @p: host address
 * @val: shifted value to store
 * @msk: mask for value to store
 *
 * Atomically store @val to @p masked by @msk.
 */
static inline void ATTRIBUTE_ATOMIC128_OPT
store_atom_insert_al16(Int128 *ps, Int128 val, Int128 msk)
{
    /*
     * GCC only implements __sync* primitives for int128 on aarch64.
     * We can do better without the barriers, and integrating the
     * arithmetic into the load-exclusive/store-conditional pair.
     */
    uint64_t tl, th, vl, vh, ml, mh;

    qemu_build_assert(!HOST_BIG_ENDIAN);
    vl = int128_getlo(val);
    vh = int128_gethi(val);
    ml = int128_getlo(msk);
    mh = int128_gethi(msk);

    asm("0: \n\t"
        "ll.d %[l], %[mem_lo]\n\t"
        "ld.d %[h], %[mem_hi]\n\t"
        "andn %[l], %[l], %[ml]\n\t"
        "andn %[h], %[h], %[mh]\n\t"
        "or %[l], %[l], %[vl]\n\t"
        "or %[h], %[h], %[vh]\n\t"
        "sc.q %[l], %[h], %[mem_lo]\n\t"
        "beq %[l], $zero, 0b\n"
        : [mem_lo] "=&ZB"(*((long *)ps)), [mem_hi] "=&ZB"(*(long *)((void *)ps + 8)),
          [l] "=&r"(tl), [h] "=&r"(th)
        : [vl] "r"(vl), [vh] "r"(vh), [ml] "r"(ml), [mh] "r"(mh));
}

#endif /* LOONGARCH_STORE_INSERT_AL16_H */
