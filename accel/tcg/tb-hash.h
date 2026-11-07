/*
 * internal execution defines for qemu
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EXEC_TB_HASH_H
#define EXEC_TB_HASH_H

#include "exec/cpu-defs.h"
#include "exec/exec-all.h"
#include "qemu/xxhash.h"
#include "tb-jmp-cache.h"

#ifdef CONFIG_SOFTMMU

/* Only the bottom TB_JMP_PAGE_BITS of the jump cache hash bits vary for
   addresses on the same page.  The top bits are the same.  This allows
   TLB invalidation to quickly clear a subset of the hash table.  */
#define TB_JMP_PAGE_BITS (TB_JMP_CACHE_BITS / 2)
#define TB_JMP_PAGE_SIZE (1 << TB_JMP_PAGE_BITS)
#define TB_JMP_ADDR_MASK (TB_JMP_PAGE_SIZE - 1)
#define TB_JMP_PAGE_MASK (TB_JMP_CACHE_SIZE - TB_JMP_PAGE_SIZE)

static inline unsigned int tb_jmp_cache_hash_page(vaddr pc)
{
    vaddr tmp;
    tmp = pc ^ (pc >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS));
    return (tmp >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS)) & TB_JMP_PAGE_MASK;
}

/*
 * The CPU state that selects a TB goes into the bits that vary within a page,
 * so that one PC reached in several states can keep several entries.  Clearing
 * a page by its TB_JMP_PAGE_MASK bits still covers all of them.
 * Mix the PC with that state before multiplying: using only its xor-folded
 * low bits leaves repeated instruction layouts competing for the same slots.
 */
static inline unsigned int tb_jmp_cache_hash_func(vaddr pc, uint64_t cs_base,
                                                  uint32_t flags)
{
    vaddr tmp;
    /*
     * Multiplicative hash using xxHash32 PRIME32_1 (0x9e3779b1, derived from
     * floor(2^32 / phi)):
     * https://xxhash.com/doc/v0.8.2/group___x_x_h32__impl.html
     */
    uint32_t state = ((uint32_t)pc ^ (uint32_t)cs_base ^ flags) * PRIME32_1;

    tmp = pc ^ (pc >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS));
    return (((tmp >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS)) & TB_JMP_PAGE_MASK)
           | (state >> (32 - TB_JMP_PAGE_BITS)));
}

#else

/* In user-mode we can get better hashing because we do not have a TLB */
static inline unsigned int tb_jmp_cache_hash_func(vaddr pc, uint64_t cs_base,
                                                  uint32_t flags)
{
    return (pc ^ (pc >> TB_JMP_CACHE_BITS)) & (TB_JMP_CACHE_SIZE - 1);
}

#endif /* CONFIG_SOFTMMU */

static inline
uint32_t tb_hash_func(tb_page_addr_t phys_pc, vaddr pc,
                      uint32_t flags, uint64_t flags2, uint32_t cf_mask)
{
    return qemu_xxhash8(phys_pc, pc, flags2, flags, cf_mask);
}

#endif
