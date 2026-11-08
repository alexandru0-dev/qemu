/*
 * Inline jump-cache lookup of the next TB for goto_ptr.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_LOOKUP_H
#define ACCEL_TCG_TB_LOOKUP_H

#include "exec/translation-block.h"
#include "tb-jmp-cache.h"
#include "tb-hash.h"

/*
 * Targets whose pc and cs_base values fit in 32 bits (with TB_JMP_CACHE_PCREL
 * disabled) can define TB_LOOKUP_LOW_WORDS to 1 in cpu-param.h to compare only
 * the low 32-bit words, saving a load and compare for each on 32-bit hosts.
 */
#if !defined(TB_LOOKUP_LOW_WORDS) || TB_JMP_CACHE_PCREL
#undef TB_LOOKUP_LOW_WORDS
#define TB_LOOKUP_LOW_WORDS 0
#endif

static inline bool tb_lookup_matches(CPUJumpCache *jc, uint32_t hash,
                                     TranslationBlock *tb, vaddr pc,
                                     uint64_t cs_base)
{
#if TB_LOOKUP_LOW_WORDS
    tcg_debug_assert(pc == (uint32_t)pc && cs_base == (uint32_t)cs_base);
    if ((uint32_t)tb->pc == (uint32_t)pc &&
        (uint32_t)tb->cs_base == (uint32_t)cs_base) {
        tcg_debug_assert(tb->pc == pc && tb->cs_base == cs_base);
        return true;
    }
    return false;
#else
    return tb_jmp_cache_pc_matches(jc, hash, tb, pc) &&
        tb->cs_base == cs_base;
#endif
}

const void *tcg_lookup_and_chain(CPUState *cpu, TranslationBlock *tb, int n,
                                 vaddr pc, uint64_t cs_base, uint32_t flags);

/* tcg_lookup_tb_ptr() without its jump-cache fast path. */
const void *tcg_lookup_tb_ptr_slow(CPUState *cpu, vaddr pc, uint64_t cs_base,
                                   uint32_t flags);

/*
 * tcg_lookup_tb_ptr(), inlined into the helper that ends a TB with a
 * goto_ptr.  @pc, @cs_base and @flags must equal what
 * cpu_get_tb_cpu_state() returns for the CPU state at the call.
 */
static inline QEMU_ALWAYS_INLINE
const void *tcg_lookup_tb_ptr_inline(CPUState *cpu, vaddr pc,
                                     uint64_t cs_base, uint32_t flags)
{
    CPUJumpCache *jc;
    TranslationBlock *tb;
    uint32_t hash;

    /*
     * By definition we've just finished a TB, so I/O is OK.
     * Avoid the possibility of calling cpu_io_recompile() if
     * a page table walk triggered by tb_lookup() calling
     * probe_access_internal() happens to touch an MMIO device.
     * The next TB, if we chain to it, will clear the flag again.
     */
    cpu->neg.can_do_io = true;

    /*
     * Fast path, without a stack frame or calls: a jump cache hit when
     * curr_cflags() would return tcg_cflags unchanged (no single-step,
     * one-insn-per-TB, nochain logging, breakpoints or exec logging),
     * which tb_lookup_cflags records.  Everything else takes the original
     * path.
     */
    hash = tb_jmp_cache_hash_func(pc, cs_base, flags);
    jc = cpu->tb_jmp_cache;
    tb = qatomic_read(&jc->array[hash].tb);
    if (likely(tb &&
               tb_lookup_matches(jc, hash, tb, pc, cs_base) &&
               tb->flags == flags &&
               tb_cflags(tb) == qatomic_read(&cpu->tb_lookup_cflags))) {
        return tb->tc.ptr;
    }
    return tcg_lookup_tb_ptr_slow(cpu, pc, cs_base, flags);
}

#endif /* ACCEL_TCG_TB_LOOKUP_H */
