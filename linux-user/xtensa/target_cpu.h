/*
 * Xtensa-specific CPU ABI and functions for linux-user
 */
#ifndef XTENSA_TARGET_CPU_H
#define XTENSA_TARGET_CPU_H

static inline void cpu_clone_regs_child(CPUXtensaState *env,
                                        target_ulong newsp,
                                        unsigned flags)
{
    /* After cpu_copy, env->regs points into the parent's phys_regs. */
    env->regs = env->phys_regs + env->sregs[WINDOW_BASE] * 4;
    if (newsp) {
        env->regs[1] = newsp;
        memmove(env->phys_regs, env->regs, 16 * sizeof(uint32_t));
        env->sregs[WINDOW_BASE] = 0;
        env->regs = env->phys_regs;
        env->sregs[WINDOW_START] = 0x1;
    }
    env->regs[2] = 0;
}

static inline void cpu_clone_regs_parent(CPUXtensaState *env, unsigned flags)
{
}

static inline void cpu_set_tls(CPUXtensaState *env, target_ulong newtls)
{
    env->uregs[THREADPTR] = newtls;
}

static inline abi_ulong get_sp_from_cpustate(CPUXtensaState *state)
{
    return state->regs[1];
}
#endif
