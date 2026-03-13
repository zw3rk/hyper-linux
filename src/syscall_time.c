/* syscall_time.c — Time and timer syscall handlers for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clock, nanosleep, gettimeofday, and interval timer operations. All
 * functions are called from syscall_dispatch() in syscall.c.
 */
#include "syscall_time.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "syscall_signal.h"
#include "guest.h"

#include <errno.h>
#include <time.h>
#include <sys/time.h>

/* Linux TIMER_ABSTIME — not defined on all macOS SDK versions */
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

/* ---------- Clock ID translation ---------- */

/* Linux dynamic CPU clock ID encoding (kernel/time/posix-cpu-timers.c):
 *
 *   clockid = ~pid << 3 | type [| CPUCLOCK_PERTHREAD_MASK]
 *
 * Where type is:  CPUCLOCK_PROF=0, CPUCLOCK_VIRT=1, CPUCLOCK_SCHED=2
 * CPUCLOCK_PERTHREAD_MASK = 4 (bit 2) — set for per-thread, clear for per-process
 * pid = 0 means "self" (current process/thread).
 *
 * Examples: -2 = per-thread SCHED (self), -6 = per-process SCHED (self)
 * GHC RTS uses these for getCurrentThreadCPUTime via pthread_getcpuclockid(). */
#define LINUX_CPUCLOCK_PERTHREAD_MASK 4
#define LINUX_CPUCLOCK_TYPE_MASK      3
#define LINUX_CPUCLOCK_PID(clk)       (~((clk) >> 3))

/* Translate Linux clock IDs to macOS.
 * Linux: REALTIME=0, MONOTONIC=1, PROCESS_CPUTIME=2, THREAD_CPUTIME=3, MONOTONIC_RAW=4
 * macOS: REALTIME=0, MONOTONIC_RAW=4, MONOTONIC=6, PROCESS_CPUTIME=12, THREAD_CPUTIME=16
 *
 * Negative clock IDs encode Linux dynamic per-process/per-thread CPU clocks.
 * We translate pid=0 (self) clocks to the macOS equivalents; for other pids
 * we return -1 (no macOS equivalent). */
static int translate_clockid(int linux_clockid) {
    switch (linux_clockid) {
    case 0: return CLOCK_REALTIME;
    case 1: return CLOCK_MONOTONIC;
    case 2: return CLOCK_PROCESS_CPUTIME_ID;
    case 3: return CLOCK_THREAD_CPUTIME_ID;
    case 4: return CLOCK_MONOTONIC_RAW;
    case 5: return CLOCK_REALTIME;          /* CLOCK_REALTIME_COARSE */
    case 6: return CLOCK_MONOTONIC;         /* CLOCK_MONOTONIC_COARSE */
    case 7: return CLOCK_MONOTONIC;         /* CLOCK_BOOTTIME (no suspend-aware clock on macOS) */
    default:
        /* Handle Linux dynamic CPU clock IDs (negative values).
         * Decode: pid = ~(clockid >> 3), perthread = clockid & 4
         * Only support pid=0 (self); other pids have no macOS equivalent. */
        if (linux_clockid < 0) {
            int pid = LINUX_CPUCLOCK_PID(linux_clockid);
            if (pid != 0) return -1;  /* Can't query other process CPU times */
            int is_perthread = linux_clockid & LINUX_CPUCLOCK_PERTHREAD_MASK;
            return is_perthread ? CLOCK_THREAD_CPUTIME_ID
                                : CLOCK_PROCESS_CPUTIME_ID;
        }
        return -1;
    }
}

/* ---------- Linux itimerval type ---------- */

/* Linux struct itimerval (same layout as macOS on LP64) */
typedef struct {
    linux_timeval_t it_interval;
    linux_timeval_t it_value;
} linux_itimerval_t;

/* ---------- Time/timer syscall handlers ---------- */

int64_t sys_clock_getres(guest_t *g, int clockid, uint64_t tp_gva) {
    int mac_clockid = translate_clockid(clockid);
    if (mac_clockid < 0) return -LINUX_EINVAL;

    /* tp may be NULL — just validates the clock ID */
    if (!tp_gva) return 0;

    struct timespec ts;
    if (clock_getres(mac_clockid, &ts) < 0)
        return linux_errno();

    linux_timespec_t lts;
    lts.tv_sec = ts.tv_sec;
    lts.tv_nsec = ts.tv_nsec;

    if (guest_write(g, tp_gva, &lts, sizeof(lts)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

int64_t sys_clock_gettime(guest_t *g, int clockid, uint64_t tp_gva) {
    struct timespec ts;
    int mac_clockid = translate_clockid(clockid);
    if (mac_clockid < 0) return -LINUX_EINVAL;
    if (clock_gettime(mac_clockid, &ts) < 0)
        return linux_errno();

    linux_timespec_t lts;
    lts.tv_sec = ts.tv_sec;
    lts.tv_nsec = ts.tv_nsec;

    if (guest_write(g, tp_gva, &lts, sizeof(lts)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

int64_t sys_nanosleep(guest_t *g, uint64_t req_gva, uint64_t rem_gva) {
    linux_timespec_t lreq;
    if (guest_read(g, req_gva, &lreq, sizeof(lreq)) < 0)
        return -LINUX_EFAULT;

    /* Linux rejects negative tv_sec or tv_nsec outside [0, 999999999] */
    if (lreq.tv_sec < 0 || lreq.tv_nsec < 0 || lreq.tv_nsec >= 1000000000LL)
        return -LINUX_EINVAL;

    struct timespec req = { .tv_sec = lreq.tv_sec, .tv_nsec = lreq.tv_nsec };
    struct timespec rem = {0};

    if (nanosleep(&req, &rem) < 0) {
        if (rem_gva) {
            linux_timespec_t lrem = { .tv_sec = rem.tv_sec, .tv_nsec = rem.tv_nsec };
            if (guest_write(g, rem_gva, &lrem, sizeof(lrem)) < 0)
                return -LINUX_EFAULT;
        }
        return linux_errno();
    }

    return 0;
}

int64_t sys_clock_nanosleep(guest_t *g, int clockid, int flags,
                            uint64_t req_gva, uint64_t rem_gva) {

    linux_timespec_t lreq;
    if (guest_read(g, req_gva, &lreq, sizeof(lreq)) < 0)
        return -LINUX_EFAULT;

    /* Linux rejects tv_nsec outside [0, 999999999] */
    if (lreq.tv_nsec < 0 || lreq.tv_nsec >= 1000000000LL)
        return -LINUX_EINVAL;

    int mac_clockid = translate_clockid(clockid);
    if (mac_clockid < 0) return -LINUX_EINVAL;

    struct timespec req;

    if (flags & TIMER_ABSTIME) {
        /* Absolute timeout: compute remaining time from clock - target */
        struct timespec now;
        if (clock_gettime(mac_clockid, &now) < 0)
            return linux_errno();

        /* Guard against signed overflow: INT64_MAX / 1e9 ≈ 9.2e9 seconds.
         * Values beyond that are absurdly far in the future; clamp. */
        if (lreq.tv_sec > INT64_MAX / 1000000000LL) return 0; /* past year 2262 */
        int64_t target_ns = lreq.tv_sec * 1000000000LL + lreq.tv_nsec;
        int64_t now_ns = now.tv_sec * 1000000000LL + now.tv_nsec;
        int64_t delta_ns = target_ns - now_ns;
        if (delta_ns <= 0) return 0;  /* Already past */

        req.tv_sec = delta_ns / 1000000000LL;
        req.tv_nsec = delta_ns % 1000000000LL;
    } else {
        req.tv_sec = lreq.tv_sec;
        req.tv_nsec = lreq.tv_nsec;
    }

    struct timespec rem = {0};
    if (nanosleep(&req, &rem) < 0) {
        /* For absolute timeouts, Linux does not write back remaining time */
        if (rem_gva && !(flags & TIMER_ABSTIME)) {
            linux_timespec_t lrem = { .tv_sec = rem.tv_sec,
                                       .tv_nsec = rem.tv_nsec };
            if (guest_write(g, rem_gva, &lrem, sizeof(lrem)) < 0)
                return -LINUX_EFAULT;
        }
        return linux_errno();
    }

    return 0;
}

int64_t sys_gettimeofday(guest_t *g, uint64_t tv_gva, uint64_t tz_gva) {
    (void)tz_gva; /* timezone is obsolete */
    struct timeval tv;
    if (gettimeofday(&tv, NULL) < 0)
        return linux_errno();

    if (tv_gva) {
        linux_timeval_t ltv;
        ltv.tv_sec = tv.tv_sec;
        ltv.tv_usec = tv.tv_usec;
        if (guest_write(g, tv_gva, &ltv, sizeof(ltv)) < 0)
            return -LINUX_EFAULT;
    }
    return 0;
}

int64_t sys_setitimer(guest_t *g, int which,
                      uint64_t new_gva, uint64_t old_gva) {
    linux_itimerval_t lnew;
    int has_new = 0;
    if (new_gva) {
        if (guest_read(g, new_gva, &lnew, sizeof(lnew)) < 0)
            return -LINUX_EFAULT;
        has_new = 1;
    }

    /* Linux rejects tv_usec outside [0, 999999] for both value and interval */
    if (has_new) {
        if (lnew.it_value.tv_usec < 0 || lnew.it_value.tv_usec >= 1000000 ||
            lnew.it_interval.tv_usec < 0 || lnew.it_interval.tv_usec >= 1000000)
            return -LINUX_EINVAL;
    }

    /* ITIMER_REAL (which=0) is emulated internally because macOS shares
     * alarm() and setitimer(ITIMER_REAL) as the same underlying timer,
     * and hl needs alarm() for its per-iteration vCPU timeout. */
    if (which == 0) {
        struct timeval val = {0}, itv = {0};
        if (has_new) {
            val = (struct timeval){ .tv_sec = (long)lnew.it_value.tv_sec,
                                    .tv_usec = (int)lnew.it_value.tv_usec };
            itv = (struct timeval){ .tv_sec = (long)lnew.it_interval.tv_sec,
                                    .tv_usec = (int)lnew.it_interval.tv_usec };
        }
        struct timeval old_val, old_itv;
        signal_set_itimer(has_new ? &val : NULL, has_new ? &itv : NULL,
                          old_gva ? &old_val : NULL,
                          old_gva ? &old_itv : NULL);
        if (old_gva) {
            linux_itimerval_t lold = {
                .it_interval = { .tv_sec = old_itv.tv_sec,
                                 .tv_usec = old_itv.tv_usec },
                .it_value    = { .tv_sec = old_val.tv_sec,
                                 .tv_usec = old_val.tv_usec },
            };
            if (guest_write(g, old_gva, &lold, sizeof(lold)) < 0)
                return -LINUX_EFAULT;
        }
        return 0;
    }

    /* ITIMER_VIRTUAL and ITIMER_PROF: stub as no-op. Forwarding to host
     * setitimer would cause SIGVTALRM/SIGPROF delivery to the hl process
     * itself, potentially killing it. Return old value as zero (disarmed). */
    if (old_gva) {
        linux_itimerval_t lold = {0};
        if (guest_write(g, old_gva, &lold, sizeof(lold)) < 0)
            return -LINUX_EFAULT;
    }
    return 0;
}

int64_t sys_getitimer(guest_t *g, int which, uint64_t val_gva) {
    /* ITIMER_REAL is emulated internally (see sys_setitimer comment). */
    if (which == 0) {
        struct timeval val, itv;
        signal_get_itimer(&val, &itv);
        linux_itimerval_t lval = {
            .it_interval = { .tv_sec = itv.tv_sec,
                             .tv_usec = itv.tv_usec },
            .it_value    = { .tv_sec = val.tv_sec,
                             .tv_usec = val.tv_usec },
        };
        if (guest_write(g, val_gva, &lval, sizeof(lval)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    struct itimerval oval;
    if (getitimer(which, &oval) < 0)
        return linux_errno();

    linux_itimerval_t lval = {
        .it_interval = { .tv_sec = oval.it_interval.tv_sec,
                         .tv_usec = oval.it_interval.tv_usec },
        .it_value    = { .tv_sec = oval.it_value.tv_sec,
                         .tv_usec = oval.it_value.tv_usec },
    };

    if (guest_write(g, val_gva, &lval, sizeof(lval)) < 0)
        return -LINUX_EFAULT;

    return 0;
}
