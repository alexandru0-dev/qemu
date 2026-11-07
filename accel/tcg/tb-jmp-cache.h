/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"
#include "cpu-param.h"

#ifndef TB_JMP_CACHE_BITS
#define TB_JMP_CACHE_BITS 12
#endif
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Only CF_PCREL TBs need the pc stored in the entry; for the others it is
 * tb->pc.  Targets that never set CF_PCREL can define TB_JMP_CACHE_PCREL to 0
 * in cpu-param.h to keep only the TB pointer, making each entry a quarter of
 * its size on 32-bit hosts and half on 64-bit hosts.  Targets may also
 * override TB_JMP_CACHE_BITS in cpu-param.h.  tcg_exec_realizefn() checks
 * CF_PCREL.
 */
#ifndef TB_JMP_CACHE_PCREL
#define TB_JMP_CACHE_PCREL 1
#endif

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
#if TB_JMP_CACHE_PCREL
        vaddr pc;
#endif
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

/* Whether the valid entry @h holding @tb is for @pc. */
static inline bool tb_jmp_cache_pc_matches(CPUJumpCache *jc, uint32_t h,
                                           TranslationBlock *tb, vaddr pc)
{
#if TB_JMP_CACHE_PCREL
    return jc->array[h].pc == pc;
#else
    return tb->pc == pc;
#endif
}

static inline void tb_jmp_cache_set(CPUJumpCache *jc, uint32_t h,
                                    TranslationBlock *tb, vaddr pc)
{
#if TB_JMP_CACHE_PCREL
    jc->array[h].pc = pc;
#endif
    qatomic_set(&jc->array[h].tb, tb);
}

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
