/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "system/cpus.h"
#include "system/qtest.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qemu/seqlock.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "exec/icount.h"
#include "system/cpu-timers-internal.h"

/*
 * ICOUNT: Instruction Counter
 *
 * this module is split off from cpu-timers because the icount part
 * is TCG-specific, and does not need to be built for other accels.
 */
static bool icount_sleep = true;
/* Arbitrarily pick 1MIPS as the minimum allowable speed.  */
#define MAX_ICOUNT_SHIFT 10

bool icount_align_option;

/* Do not count executed instructions */
ICountMode use_icount = ICOUNT_DISABLED;

static void icount_enable_precise(void)
{
    /* Fixed conversion of insn to ns via "shift" option */
    use_icount = ICOUNT_PRECISE;
}

static void icount_enable_adaptive(void)
{
    /* Runtime adaptive algorithm to compute shift */
    use_icount = ICOUNT_ADAPTATIVE;
}

/*
 * The current number of executed instructions is based on what we
 * originally budgeted minus the current state of the decrementing
 * icount counters in extra/u16.low.
 */
static int64_t icount_get_executed(CPUState *cpu)
{
    return (cpu->icount_budget -
            (cpu->neg.icount_decr.u16.low + cpu->icount_extra));
}

/*
 * Update the global shared timer_state.qemu_icount to take into
 * account executed instructions. This is done by the TCG vCPU
 * thread so the main-loop can see time has moved forward.
 */
static void icount_update_locked(CPUState *cpu)
{
    int64_t executed = icount_get_executed(cpu);
    cpu->icount_budget -= executed;

    qatomic_set(&timers_state.qemu_icount,
                timers_state.qemu_icount + executed);
}

/*
 * Update the global shared timer_state.qemu_icount to take into
 * account executed instructions. This is done by the TCG vCPU
 * thread so the main-loop can see time has moved forward.
 */
void icount_update(CPUState *cpu)
{
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    icount_update_locked(cpu);
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static int64_t icount_get_raw_locked(void)
{
    CPUState *cpu = current_cpu;

    if (cpu && cpu->running) {
        if (!cpu->neg.can_do_io) {
            error_report("Bad icount read");
            exit(1);
        }
        /* Take into account what has run */
        icount_update_locked(cpu);
    }
    /* The read is protected by the seqlock, but needs atomic to avoid UB */
    return qatomic_read(&timers_state.qemu_icount);
}

static int64_t icount_get_locked(void)
{
    int64_t icount = icount_get_raw_locked();
    return qatomic_read(&timers_state.qemu_icount_bias) + icount_to_ns(icount);
}

int64_t icount_get_raw(void)
{
    int64_t icount;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        icount = icount_get_raw_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return icount;
}

/* Return the virtual CPU time, based on the instruction counter.  */
int64_t icount_get(void)
{
    int64_t icount;
    unsigned start;

    do {
        start = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        icount = icount_get_locked();
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, start));

    return icount;
}

int64_t icount_to_ns(int64_t icount)
{
    return icount << qatomic_read(&timers_state.icount_time_shift);
}

/*
 * Correlation between real and virtual time is always going to be
 * fairly approximate, so ignore small variation.
 * When the guest is idle real and virtual time will be aligned in
 * the IO wait loop.
 */
#define ICOUNT_WOBBLE (NANOSECONDS_PER_SECOND / 10)

static void icount_adjust(void)
{
    int64_t cur_time;
    int64_t cur_icount;
    int64_t delta;

    /* If the VM is not running, then do nothing.  */
    if (!runstate_is_running()) {
        return;
    }

    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    cur_time = REPLAY_CLOCK_LOCKED(REPLAY_CLOCK_VIRTUAL_RT,
                                   cpu_get_clock_locked());
    cur_icount = icount_get_locked();

    delta = cur_icount - cur_time;
    /* FIXME: This is a very crude algorithm, somewhat prone to oscillation.  */
    if (delta > 0
        && timers_state.last_delta + ICOUNT_WOBBLE < delta * 2
        && timers_state.icount_time_shift > 0) {
        /* The guest is getting too far ahead.  Slow time down.  */
        qatomic_set(&timers_state.icount_time_shift,
                    timers_state.icount_time_shift - 1);
    }
    if (delta < 0
        && timers_state.last_delta - ICOUNT_WOBBLE > delta * 2
        && timers_state.icount_time_shift < MAX_ICOUNT_SHIFT) {
        /* The guest is getting too far behind.  Speed time up.  */
        qatomic_set(&timers_state.icount_time_shift,
                    timers_state.icount_time_shift + 1);
    }
    timers_state.last_delta = delta;
    qatomic_set(&timers_state.qemu_icount_bias,
                cur_icount - (timers_state.qemu_icount
                              << timers_state.icount_time_shift));
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
}

static void icount_adjust_rt(void *opaque)
{
    timer_mod(timers_state.icount_rt_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    icount_adjust();
}

static void icount_adjust_vm(void *opaque)
{
    timer_mod(timers_state.icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
    icount_adjust();
}

int64_t icount_round(int64_t count)
{
    int shift = qatomic_read(&timers_state.icount_time_shift);
    return (count + (1 << shift) - 1) >> shift;
}

/*
 * Idle pacing for fixed-shift icount with sleep=on (-icount pace=on).
 *
 * An idle warp only covers the real time spent sleeping.  Busy periods advance
 * QEMU_CLOCK_VIRTUAL by their icount value, whatever real time they took, and
 * thread wake-up latency around each warp is never returned to the guest.  So
 * a guest that executes faster than its icount rate runs ahead of real time,
 * and a mostly idle, timer-heavy guest falls persistently behind it.
 *
 * Track how far QEMU_CLOCK_VIRTUAL lags QEMU_CLOCK_VIRTUAL_RT and shorten or
 * lengthen idle sleeps to remove it.  Repayment only happens while all vCPUs
 * are idle and never crosses the next timer deadline, so guest timers still
 * fire in order with guest code running between them.  The difference is
 * bounded: anything beyond the bound, such as a host stall, is forgotten
 * rather than repaid, and cannot compress the guest's timers for longer than
 * the bound allows.
 */
#define ICOUNT_PACE_MAX_DRIFT_NS (100 * SCALE_MS)

static bool icount_pace;
static bool icount_pace_started;
static int64_t icount_pace_offset;

static bool icount_pace_enabled(void)
{
    return icount_pace && icount_enabled() == ICOUNT_PRECISE &&
           replay_mode == REPLAY_MODE_NONE;
}

/*
 * Pick a new reference when the VM (re)starts: loadvm or an incoming migration
 * moves QEMU_CLOCK_VIRTUAL, and that difference must not be repaid.
 */
static void icount_pace_vm_state_change(void *opaque, bool running,
                                        RunState state)
{
    if (running) {
        seqlock_write_lock(&timers_state.vm_clock_seqlock,
                           &timers_state.vm_clock_lock);
        icount_pace_started = false;
        seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                             &timers_state.vm_clock_lock);
    }
}

/* Real minus guest time, in [-bound, +bound].  Requires the clock seqlock. */
static int64_t icount_pace_lag_locked(int64_t clock)
{
    int64_t lag;

    if (!icount_pace_started) {
        icount_pace_offset = clock - icount_get_locked();
        icount_pace_started = true;
    }
    lag = clock - icount_get_locked() - icount_pace_offset;
    if (lag > ICOUNT_PACE_MAX_DRIFT_NS) {
        icount_pace_offset += lag - ICOUNT_PACE_MAX_DRIFT_NS;
        lag = ICOUNT_PACE_MAX_DRIFT_NS;
    } else if (lag < -ICOUNT_PACE_MAX_DRIFT_NS) {
        icount_pace_offset += lag + ICOUNT_PACE_MAX_DRIFT_NS;
        lag = -ICOUNT_PACE_MAX_DRIFT_NS;
    }
    return lag;
}

/* Ends a pending idle warp; returns false if none was pending. */
static bool icount_warp_rt_account(void)
{
    unsigned seq;
    int64_t warp_start;
    int64_t deadline;

    /*
     * The icount_warp_timer is rescheduled soon after vm_clock_warp_start
     * changes from -1 to another value, so the race here is okay.
     */
    do {
        seq = seqlock_read_begin(&timers_state.vm_clock_seqlock);
        warp_start = timers_state.vm_clock_warp_start;
    } while (seqlock_read_retry(&timers_state.vm_clock_seqlock, seq));

    if (warp_start == -1) {
        return false;
    }

    deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                          ~QEMU_TIMER_ATTR_EXTERNAL);
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (runstate_is_running()) {
        int64_t clock = REPLAY_CLOCK_LOCKED(REPLAY_CLOCK_VIRTUAL_RT,
                                            cpu_get_clock_locked());
        int64_t warp_delta;

        warp_delta = clock - timers_state.vm_clock_warp_start;
        if (icount_pace_enabled()) {
            warp_delta = MAX(icount_pace_lag_locked(clock), 0);
        }
        if (deadline >= 0) {
            warp_delta = MIN(warp_delta, deadline);
        }
        if (icount_enabled() == ICOUNT_ADAPTATIVE) {
            /*
             * In adaptive mode, do not let QEMU_CLOCK_VIRTUAL run too far
             * ahead of real time (it might already be ahead so careful not
             * to go backwards).
             */
            int64_t cur_icount = icount_get_locked();
            int64_t delta = clock - cur_icount;

            if (delta < 0) {
                delta = 0;
            }
            warp_delta = MIN(warp_delta, delta);
        }
        qatomic_set(&timers_state.qemu_icount_bias,
                    timers_state.qemu_icount_bias + warp_delta);
    }
    timers_state.vm_clock_warp_start = -1;
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    return true;
}

static void icount_warp_rt(void)
{
    if (icount_warp_rt_account() && qemu_clock_expired(QEMU_CLOCK_VIRTUAL)) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}

/*
 * Set, under the BQL, while the vCPU thread sleeps through an idle period
 * itself. The BQL is released during that sleep, so main_loop_wait() could
 * run icount_start_warp_timer() and arm a second warp for the same period;
 * it leaves the period to the vCPU instead.
 */
static bool icount_vcpu_idle_sleep;

/*
 * sleep=on counterpart of icount_warp_idle_in_vcpu(): the vCPU thread sleeps
 * through an idle period itself instead of waking the main loop to arm the warp
 * timer and waiting for it to warp and kick back. It starts the warp as
 * icount_start_warp_timer() would and returns the real time to sleep, including
 * the pacing correction; it then waits on its halt condition, which kicks still
 * interrupt, and calls icount_idle_sleep_end(). The warp only covers real time
 * that passed, so an early wake-up leaves the rest for the next idle period.
 *
 * Returns -1, leaving idle periods to the main loop, unless icount is precise
 * and sleeps, record/replay and align are off, the VM runs, and a timer is
 * pending.
 */
int64_t icount_idle_sleep_begin(void)
{
    int64_t clock, deadline, sleep_ns;

    if (!icount_sleep || icount_enabled() != ICOUNT_PRECISE ||
        icount_align_option || replay_mode != REPLAY_MODE_NONE ||
        !runstate_is_running() || qtest_enabled()) {
        return -1;
    }

    /* As in icount_warp_idle_in_vcpu(), zero is another AioContext's timer. */
    deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                          ~QEMU_TIMER_ATTR_EXTERNAL);
    if (deadline <= 0) {
        return -1;
    }

    clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
    sleep_ns = deadline;
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    if (timers_state.vm_clock_warp_start == -1
        || timers_state.vm_clock_warp_start > clock) {
        timers_state.vm_clock_warp_start = clock;
    }
    if (icount_pace_enabled()) {
        sleep_ns -= icount_pace_lag_locked(clock);
    }
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
    icount_vcpu_idle_sleep = true;
    return MAX(sleep_ns, 0);
}

/*
 * Ends the warp after the sleep. The vCPU loop runs the timers now due, so the
 * main loop is not notified.
 */
void icount_idle_sleep_end(void)
{
    icount_vcpu_idle_sleep = false;
    icount_warp_rt_account();
}

static void icount_timer_cb(void *opaque)
{
    /*
     * No need for a checkpoint because the timer already synchronizes
     * with CHECKPOINT_CLOCK_VIRTUAL_RT.
     */
    icount_warp_rt();
}

/*
 * With sleep=off an idle warp only moves QEMU_CLOCK_VIRTUAL to the next
 * deadline. The vCPU thread holds the BQL when it finds every CPU idle, so it
 * can do that itself instead of waking the main loop, sleeping, and waiting
 * for the main loop to warp and kick it back: two context switches and a
 * main-loop pass per guest idle period. The caller runs the timers now due.
 *
 * Returns false, leaving the warp to the main loop, when sleeping is enabled,
 * in record/replay or qtest, when the VM is not running, or when no timer is
 * pending.
 */
bool icount_warp_idle_in_vcpu(void)
{
    int64_t deadline;

    /* As icount_start_warp_timer(): qtest commands advance the clock. */
    if (icount_sleep || replay_mode != REPLAY_MODE_NONE ||
        !runstate_is_running() || qtest_enabled()) {
        return false;
    }

    /*
     * The vCPU loop has just run the main loop's due timers, so a deadline of
     * zero is a timer only another AioContext runs: wait for it the usual way
     * rather than spin.
     */
    deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                          ~QEMU_TIMER_ATTR_EXTERNAL);
    if (deadline <= 0) {
        return false;
    }
    seqlock_write_lock(&timers_state.vm_clock_seqlock,
                       &timers_state.vm_clock_lock);
    qatomic_set(&timers_state.qemu_icount_bias,
                    timers_state.qemu_icount_bias + deadline);
    seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                         &timers_state.vm_clock_lock);
    return true;
}

void icount_start_warp_timer(void)
{
    int64_t clock;
    int64_t deadline;

    assert(icount_enabled());

    /*
     * Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        return;
    }

    /* The vCPU thread is sleeping through this idle period itself. */
    if (icount_vcpu_idle_sleep) {
        return;
    }

    if (replay_mode != REPLAY_MODE_PLAY) {
        if (!all_cpu_threads_idle()) {
            return;
        }

        if (qtest_enabled()) {
            /* When testing, qtest commands advance icount.  */
            return;
        }

        replay_checkpoint(CHECKPOINT_CLOCK_WARP_START);
    } else {
        /* warp clock deterministically in record/replay mode */
        if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_START)) {
            /*
             * vCPU is sleeping and warp can't be started.
             * It is probably a race condition: notification sent
             * to vCPU was processed in advance and vCPU went to sleep.
             * Therefore we have to wake it up for doing something.
             */
            if (replay_has_event()) {
                qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
            }
            return;
        }
    }

    /* We want to use the earliest deadline from ALL vm_clocks */
    clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL_RT);
    deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                          ~QEMU_TIMER_ATTR_EXTERNAL);
    if (deadline < 0) {
        if (!icount_sleep) {
            warn_report_once("icount sleep disabled and no active timers");
        }
        return;
    }

    if (deadline > 0) {
        /*
         * Ensure QEMU_CLOCK_VIRTUAL proceeds even when the virtual CPU goes to
         * sleep.  Otherwise, the CPU might be waiting for a future timer
         * interrupt to wake it up, but the interrupt never comes because
         * the vCPU isn't running any insns and thus doesn't advance the
         * QEMU_CLOCK_VIRTUAL.
         */
        if (!icount_sleep) {
            /*
             * We never let VCPUs sleep in no sleep icount mode.
             * If there is a pending QEMU_CLOCK_VIRTUAL timer we just advance
             * to the next QEMU_CLOCK_VIRTUAL event and notify it.
             * It is useful when we want a deterministic execution time,
             * isolated from host latencies.
             */
            seqlock_write_lock(&timers_state.vm_clock_seqlock,
                               &timers_state.vm_clock_lock);
            qatomic_set(&timers_state.qemu_icount_bias,
                        timers_state.qemu_icount_bias + deadline);
            seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                                 &timers_state.vm_clock_lock);
            qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
        } else {
            /*
             * We do stop VCPUs and only advance QEMU_CLOCK_VIRTUAL after some
             * "real" time, (related to the time left until the next event) has
             * passed. The QEMU_CLOCK_VIRTUAL_RT clock will do this.
             * This avoids that the warps are visible externally; for example,
             * you will not be sending network packets continuously instead of
             * every 100ms.
             */
            int64_t sleep_ns = deadline;

            seqlock_write_lock(&timers_state.vm_clock_seqlock,
                               &timers_state.vm_clock_lock);
            if (timers_state.vm_clock_warp_start == -1
                || timers_state.vm_clock_warp_start > clock) {
                timers_state.vm_clock_warp_start = clock;
            }
            if (icount_pace_enabled()) {
                sleep_ns -= icount_pace_lag_locked(clock);
            }
            seqlock_write_unlock(&timers_state.vm_clock_seqlock,
                                 &timers_state.vm_clock_lock);
            timer_mod_anticipate(timers_state.icount_warp_timer,
                                 clock + sleep_ns);
        }
    } else if (deadline == 0) {
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}

void icount_account_warp_timer(void)
{
    if (!icount_sleep) {
        return;
    }

    /*
     * Nothing to do if the VM is stopped: QEMU_CLOCK_VIRTUAL timers
     * do not fire, so computing the deadline does not make sense.
     */
    if (!runstate_is_running()) {
        return;
    }

    replay_async_events();

    /* warp clock deterministically in record/replay mode */
    if (!replay_checkpoint(CHECKPOINT_CLOCK_WARP_ACCOUNT)) {
        return;
    }

    timer_del(timers_state.icount_warp_timer);
    icount_warp_rt();
}

bool icount_configure(QemuOpts *opts, Error **errp)
{
    const char *option = qemu_opt_get(opts, "shift");
    bool sleep = qemu_opt_get_bool(opts, "sleep", true);
    bool align = qemu_opt_get_bool(opts, "align", false);
    bool pace = qemu_opt_get_bool(opts, "pace", false);
    long time_shift = -1;

    if (!option) {
        if (qemu_opt_get(opts, "align") != NULL) {
            error_setg(errp, "Please specify shift option when using align");
            return false;
        }
        if (qemu_opt_get(opts, "pace") != NULL) {
            error_setg(errp, "Please specify shift option when using pace");
            return false;
        }
        return true;
    }

    if (align && !sleep) {
        error_setg(errp, "align=on and sleep=off are incompatible");
        return false;
    }

    if (pace && (!sleep || align || strcmp(option, "auto") == 0)) {
        error_setg(errp, "pace=on requires a fixed shift and sleep=on, "
                   "and is incompatible with align=on");
        return false;
    }

    if (strcmp(option, "auto") != 0) {
        if (qemu_strtol(option, NULL, 0, &time_shift) < 0
            || time_shift < 0 || time_shift > MAX_ICOUNT_SHIFT) {
            error_setg(errp, "icount: Invalid shift value");
            return false;
        }
    } else if (icount_align_option) {
        error_setg(errp, "shift=auto and align=on are incompatible");
        return false;
    } else if (!icount_sleep) {
        error_setg(errp, "shift=auto and sleep=off are incompatible");
        return false;
    }

    icount_sleep = sleep;
    if (icount_sleep) {
        timers_state.icount_warp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL_RT,
                                         icount_timer_cb, NULL);
    }

    icount_align_option = align;

    icount_pace = pace;
    if (icount_pace) {
        qemu_add_vm_change_state_handler(icount_pace_vm_state_change, NULL);
    }

    if (time_shift >= 0) {
        timers_state.icount_time_shift = time_shift;
        icount_enable_precise();
        return true;
    }

    icount_enable_adaptive();

    /*
     * 125MIPS seems a reasonable initial guess at the guest speed.
     * It will be corrected fairly quickly anyway.
     */
    timers_state.icount_time_shift = 3;

    /*
     * Have both realtime and virtual time triggers for speed adjustment.
     * The realtime trigger catches emulated time passing too slowly,
     * the virtual time trigger catches emulated time passing too fast.
     * Realtime triggers occur even when idle, so use them less frequently
     * than VM triggers.
     */
    timers_state.vm_clock_warp_start = -1;
    timers_state.icount_rt_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL_RT,
                                   icount_adjust_rt, NULL);
    timer_mod(timers_state.icount_rt_timer,
                   qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL_RT) + 1000);
    timers_state.icount_vm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        icount_adjust_vm, NULL);
    timer_mod(timers_state.icount_vm_timer,
                   qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                   NANOSECONDS_PER_SECOND / 10);
    return true;
}

void icount_notify_exit(void)
{
    assert(icount_enabled());

    if (current_cpu) {
        qemu_cpu_kick(current_cpu);
        qemu_clock_notify(QEMU_CLOCK_VIRTUAL);
    }
}
