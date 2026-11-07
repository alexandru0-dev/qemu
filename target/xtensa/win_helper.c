/*
 * Copyright (c) 2011 - 2019, Max Filippov, Open Source and Linux Lab.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Open Source and Linux Lab nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "qemu/host-utils.h"
#include "exec/exec-all.h"

static void copy_window_from_phys(CPUXtensaState *env,
                                  uint32_t window, uint32_t phys, uint32_t n)
{
    assert(phys < env->config->nareg);
    if (phys + n <= env->config->nareg) {
        memcpy(env->regs + window, env->phys_regs + phys,
               n * sizeof(uint32_t));
    } else {
        uint32_t n1 = env->config->nareg - phys;
        memcpy(env->regs + window, env->phys_regs + phys,
               n1 * sizeof(uint32_t));
        memcpy(env->regs + window + n1, env->phys_regs,
               (n - n1) * sizeof(uint32_t));
    }
}

static void copy_phys_from_window(CPUXtensaState *env,
                                  uint32_t phys, uint32_t window, uint32_t n)
{
    assert(phys < env->config->nareg);
    if (phys + n <= env->config->nareg) {
        memcpy(env->phys_regs + phys, env->regs + window,
               n * sizeof(uint32_t));
    } else {
        uint32_t n1 = env->config->nareg - phys;
        memcpy(env->phys_regs + phys, env->regs + window,
               n1 * sizeof(uint32_t));
        memcpy(env->phys_regs, env->regs + window + n1,
               (n - n1) * sizeof(uint32_t));
    }
}

static inline unsigned windowbase_bound(unsigned a, const CPUXtensaState *env)
{
    return a & (env->config->nareg / 4 - 1);
}

static inline unsigned windowstart_bit(unsigned a, const CPUXtensaState *env)
{
    return 1 << windowbase_bound(a, env);
}

void xtensa_sync_window_from_phys(CPUXtensaState *env)
{
    copy_window_from_phys(env, 0, env->sregs[WINDOW_BASE] * 4, 16);
}

void xtensa_sync_phys_from_window(CPUXtensaState *env)
{
    copy_phys_from_window(env, env->sregs[WINDOW_BASE] * 4, 0, 16);
}

/*
 * env->regs holds the current window and is authoritative for it; phys_regs is
 * authoritative for every other physical register (readers of phys_regs call
 * xtensa_sync_phys_from_window first). A rotation by up to three quads keeps
 * the overlapping registers in env->regs, so only the quads leaving the window
 * are written back and only the quads entering it are loaded.
 */
static inline QEMU_ALWAYS_INLINE
void rotate_window_abs_inline(CPUXtensaState *env, uint32_t position)
{
    uint32_t nquad = env->config->nareg / 4;
    uint32_t old = env->sregs[WINDOW_BASE];
    uint32_t new = windowbase_bound(position, env);
    uint32_t d = (new - old) & (nquad - 1);

    env->sregs[WINDOW_BASE] = new;
    if (d == 0) {
        return;
    }
    /* Copy whole quads with constant sizes so that no library call is made. */
    if (nquad >= 8 && d <= 3) {
        int q;

        for (q = 0; q < d; q++) {
            memcpy(env->phys_regs + (((old + q) & (nquad - 1)) * 4),
                   env->regs + q * 4, 4 * sizeof(uint32_t));
        }
        for (q = 0; q < 4 - d; q++) {
            memcpy(env->regs + q * 4, env->regs + (q + d) * 4,
                   4 * sizeof(uint32_t));
        }
        for (q = 4 - d; q < 4; q++) {
            memcpy(env->regs + q * 4,
                   env->phys_regs + (((new + q) & (nquad - 1)) * 4),
                   4 * sizeof(uint32_t));
        }
    } else if (nquad >= 8 && d >= nquad - 3) {
        uint32_t e = nquad - d;
        int q;

        for (q = 4 - e; q < 4; q++) {
            memcpy(env->phys_regs + (((old + q) & (nquad - 1)) * 4),
                   env->regs + q * 4, 4 * sizeof(uint32_t));
        }
        for (q = 3; q >= (int)e; q--) {
            memcpy(env->regs + q * 4, env->regs + (q - e) * 4,
                   4 * sizeof(uint32_t));
        }
        for (q = 0; q < e; q++) {
            memcpy(env->regs + q * 4,
                   env->phys_regs + (((new + q) & (nquad - 1)) * 4),
                   4 * sizeof(uint32_t));
        }
    } else {
        copy_phys_from_window(env, old * 4, 0, 16);
        copy_window_from_phys(env, 0, new * 4, 16);
    }
}

static void xtensa_rotate_window_abs(CPUXtensaState *env, uint32_t position)
{
    rotate_window_abs_inline(env, position);
}

void xtensa_rotate_window(CPUXtensaState *env, uint32_t delta)
{
    xtensa_rotate_window_abs(env, env->sregs[WINDOW_BASE] + delta);
}

void HELPER(sync_windowbase)(CPUXtensaState *env)
{
    xtensa_rotate_window_abs(env, env->windowbase_next);
}

static inline QEMU_ALWAYS_INLINE
void entry_inline(CPUXtensaState *env, uint32_t s, uint32_t imm)
{
    int callinc = (env->sregs[PS] & PS_CALLINC) >> PS_CALLINC_SHIFT;

    env->regs[(callinc << 2) | (s & 3)] = env->regs[s] - imm;
    env->windowbase_next = env->sregs[WINDOW_BASE] + callinc;
    env->sregs[WINDOW_START] |= windowstart_bit(env->windowbase_next, env);
}

void HELPER(entry)(CPUXtensaState *env, uint32_t pc, uint32_t s, uint32_t imm)
{
    entry_inline(env, s, imm);
}

void HELPER(window_check)(CPUXtensaState *env, uint32_t pc, uint32_t w)
{
    uint32_t windowbase = windowbase_bound(env->sregs[WINDOW_BASE], env);
    uint32_t windowstart = xtensa_replicate_windowstart(env) >>
        (env->sregs[WINDOW_BASE] + 1);
    uint32_t n = ctz32(windowstart) + 1;

    assert(n <= w);

    xtensa_rotate_window(env, n);
    env->sregs[PS] = (env->sregs[PS] & ~PS_OWB) |
        (windowbase << PS_OWB_SHIFT) | PS_EXCM;
    env->sregs[EPC1] = env->pc = pc;

    switch (ctz32(windowstart >> n)) {
    case 0:
        HELPER(exception)(env, EXC_WINDOW_OVERFLOW4);
        break;
    case 1:
        HELPER(exception)(env, EXC_WINDOW_OVERFLOW8);
        break;
    default:
        HELPER(exception)(env, EXC_WINDOW_OVERFLOW12);
        break;
    }
}

static inline QEMU_ALWAYS_INLINE
void test_ill_retw_inline(CPUXtensaState *env, uint32_t pc)
{
    int n = (env->regs[0] >> 30) & 0x3;
    int m = 0;
    uint32_t windowbase = windowbase_bound(env->sregs[WINDOW_BASE], env);
    uint32_t windowstart = env->sregs[WINDOW_START];

    if (windowstart & windowstart_bit(windowbase - 1, env)) {
        m = 1;
    } else if (windowstart & windowstart_bit(windowbase - 2, env)) {
        m = 2;
    } else if (windowstart & windowstart_bit(windowbase - 3, env)) {
        m = 3;
    }

    if (n == 0 || (m != 0 && m != n)) {
        qemu_log_mask(LOG_GUEST_ERROR, "Illegal retw instruction(pc = %08x), "
                      "PS = %08x, m = %d, n = %d\n",
                      pc, env->sregs[PS], m, n);
        HELPER(exception_cause)(env, pc, ILLEGAL_INSTRUCTION_CAUSE);
    }
}

void HELPER(test_ill_retw)(CPUXtensaState *env, uint32_t pc)
{
    test_ill_retw_inline(env, pc);
}

static inline QEMU_ALWAYS_INLINE
void test_underflow_retw_inline(CPUXtensaState *env, uint32_t pc)
{
    int n = (env->regs[0] >> 30) & 0x3;

    if (!(env->sregs[WINDOW_START] &
          windowstart_bit(env->sregs[WINDOW_BASE] - n, env))) {
        uint32_t windowbase = windowbase_bound(env->sregs[WINDOW_BASE], env);

        xtensa_rotate_window(env, -n);
        /* window underflow */
        env->sregs[PS] = (env->sregs[PS] & ~PS_OWB) |
            (windowbase << PS_OWB_SHIFT) | PS_EXCM;
        env->sregs[EPC1] = env->pc = pc;

        if (n == 1) {
            HELPER(exception)(env, EXC_WINDOW_UNDERFLOW4);
        } else if (n == 2) {
            HELPER(exception)(env, EXC_WINDOW_UNDERFLOW8);
        } else if (n == 3) {
            HELPER(exception)(env, EXC_WINDOW_UNDERFLOW12);
        }
    }
}

void HELPER(test_underflow_retw)(CPUXtensaState *env, uint32_t pc)
{
    test_underflow_retw_inline(env, pc);
}

void HELPER(retw)(CPUXtensaState *env, uint32_t a0)
{
    int n = (a0 >> 30) & 0x3;

    xtensa_rotate_window(env, -n);
}

/*
 * test_ill_retw, test_underflow_retw and retw of a single-op RETW in one call,
 * with the checks and the window rotation inlined.
 */
void HELPER(retw_checked)(CPUXtensaState *env, uint32_t pc)
{
    test_ill_retw_inline(env, pc);
    test_underflow_retw_inline(env, pc);
    env->sregs[WINDOW_START] &= ~(1u << env->sregs[WINDOW_BASE]);
    rotate_window_abs_inline(env, env->sregs[WINDOW_BASE] -
                             ((env->regs[0] >> 30) & 0x3));
}

/*
 * helper_lookup_tb_ptr for a jump to @pc whose other effects on the TB
 * flags are known at translate time.  Within a TB the flag-relevant state
 * only changes in the instruction that ends it, so @flags is the TB's own
 * flags as adjusted by the translator for that instruction, plus the
 * XTENSA_LOOKUP_* requests for what it cannot know: the window field after
 * a window rotation and cs_base, which depends on the target page.  YIELD
 * is read here, as other code may set it at any time.
 */
static inline QEMU_ALWAYS_INLINE
const void *lookup_next_tb(CPUXtensaState *env, uint32_t pc, uint32_t flags)
{
    uint64_t cs_base = 0;

    if (flags & XTENSA_LOOKUP_WINDOW) {
        uint32_t ws = env->sregs[WINDOW_START];
        uint32_t windowstart = (ws | (ws << env->config->nareg / 4)) >>
            (env->sregs[WINDOW_BASE] + 1);

        flags |= ctz32(windowstart | 0x8) << XTENSA_TBFLAG_WINDOW_SHIFT;
    }
    if (flags & XTENSA_LOOKUP_LOOP) {
        uint32_t lend = env->sregs[LEND];
        uint32_t lend_dist = lend - (pc & TARGET_PAGE_MASK);

        if (lend_dist < TARGET_PAGE_SIZE + env->config->max_insn_size) {
            uint32_t lbeg_off = lend - env->sregs[LBEG];

            cs_base = lend_dist;
            if (lbeg_off < 256) {
                cs_base |= lbeg_off << XTENSA_CSBASE_LBEG_OFF_SHIFT;
            }
        }
    }
    flags &= ~(XTENSA_LOOKUP_WINDOW | XTENSA_LOOKUP_LOOP);
    if (env->yield_needed) {
        flags |= XTENSA_TBFLAG_YIELD;
    }
#ifdef CONFIG_DEBUG_TCG
    {
        vaddr full_pc;
        uint64_t full_cs_base;
        uint32_t full_flags;

        cpu_get_tb_cpu_state(env, &full_pc, &full_cs_base, &full_flags);
        if (full_pc != pc || full_cs_base != cs_base || full_flags != flags) {
            error_report("xtensa: next TB state mismatch: pc %08x/%08x "
                         "cs_base %"PRIx64"/%"PRIx64" flags %08x/%08x",
                         pc, (uint32_t)full_pc, cs_base, full_cs_base,
                         flags, full_flags);
            abort();
        }
    }
#endif
    return tcg_lookup_tb_ptr(env_cpu(env), pc, cs_base, flags);
}

const void *HELPER(lookup_next_tb)(CPUXtensaState *env, uint32_t pc,
                                   uint32_t flags)
{
    return lookup_next_tb(env, pc, flags);
}

/*
 * retw_checked followed by the jump to the return address and the lookup of
 * the TB there, in one call.  @flags carries XTENSA_LOOKUP_WINDOW.
 */
const void *HELPER(retw_lookup)(CPUXtensaState *env, uint32_t pc,
                                uint32_t flags)
{
    uint32_t a0 = env->regs[0];
    uint32_t next_pc = (pc & 0xc0000000) | (a0 & 0x3fffffff);

    test_ill_retw_inline(env, pc);
    test_underflow_retw_inline(env, pc);
    env->sregs[WINDOW_START] &= ~(1u << env->sregs[WINDOW_BASE]);
    rotate_window_abs_inline(env, env->sregs[WINDOW_BASE] -
                             ((a0 >> 30) & 0x3));
    env->pc = next_pc;
    return lookup_next_tb(env, next_pc, flags);
}

/* entry followed by the window rotation gen_postprocess would emit. */
void HELPER(entry_rotate)(CPUXtensaState *env, uint32_t pc, uint32_t s,
                          uint32_t imm)
{
    entry_inline(env, s, imm);
    rotate_window_abs_inline(env, env->windowbase_next);
}

void xtensa_restore_owb(CPUXtensaState *env)
{
    xtensa_rotate_window_abs(env, (env->sregs[PS] & PS_OWB) >> PS_OWB_SHIFT);
}

void HELPER(restore_owb)(CPUXtensaState *env)
{
    xtensa_restore_owb(env);
}

void HELPER(movsp)(CPUXtensaState *env, uint32_t pc)
{
    if ((env->sregs[WINDOW_START] &
         (windowstart_bit(env->sregs[WINDOW_BASE] - 3, env) |
          windowstart_bit(env->sregs[WINDOW_BASE] - 2, env) |
          windowstart_bit(env->sregs[WINDOW_BASE] - 1, env))) == 0) {
        HELPER(exception_cause)(env, pc, ALLOCA_CAUSE);
    }
}
