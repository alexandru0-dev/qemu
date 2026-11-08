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
               tb_jmp_cache_pc_matches(jc, hash, tb, pc) &&
               tb->cs_base == cs_base &&
               tb->flags == flags &&
               tb_cflags(tb) == qatomic_read(&cpu->tb_lookup_cflags))) {
        return tb->tc.ptr;
    }
    return tcg_lookup_tb_ptr_slow(cpu, pc, cs_base, flags);
}

#endif /* ACCEL_TCG_TB_LOOKUP_H */
