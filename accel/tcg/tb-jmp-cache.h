/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#define TB_JMP_CACHE_BITS 12
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * LATX CS64 compact shadow entry.  tcptr == NULL denotes an empty entry.
 * The cache is per-CPU, so its generated lookup needs no memory barrier.
 * Keep the 32-byte stride so the hot path can form an entry address with a
 * shift and add even though no TranslationBlock pointer is retained.
 */
typedef struct LatxNJCEntry {
    const void *tcptr;
    vaddr pc;
    uint32_t flags;
    uint16_t observed_call_entry;
    uint16_t direct_entry_off;
    /* Native ABI entry for CALLs and compatible indirect tail jumps. */
    const void *call_tcptr;
} LatxNJCEntry;

typedef struct LatxNJC {
    struct rcu_head rcu;
    LatxNJCEntry array[];
} LatxNJC;

/*
 * Accessed in parallel; all accesses to 'tb' must be atomic.
 * For CF_PCREL, accesses to 'pc' must be protected by a
 * load_acquire/store_release to 'tb'.
 */
#ifdef LATX_PCREL
typedef struct CPUJumpCacheEntry {
    TranslationBlock *tb;
    vaddr pc;
} CPUJumpCacheEntry;

struct CPUJumpCache {
    struct rcu_head rcu;
    CPUJumpCacheEntry array[];
};
#else
typedef struct CPUJumpCacheEntry {
    TranslationBlock *tb;
} CPUJumpCacheEntry;

struct CPUJumpCache {
    struct rcu_head rcu;
    CPUJumpCacheEntry array[];
};
#endif
#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
