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

/* ---------- Clock ID translation ---------- */

/* Translate Linux clock IDs to macOS.
 * Linux: REALTIME=0, MONOTONIC=1, PROCESS_CPUTIME=2, THREAD_CPUTIME=3, MONOTONIC_RAW=4
 * macOS: REALTIME=0, MONOTONIC_RAW=4, MONOTONIC=6, PROCESS_CPUTIME=12, THREAD_CPUTIME=16 */
static int translate_clockid(int linux_clockid) {
    switch (linux_clockid) {
    case 0: return CLOCK_REALTIME;
    case 1: return CLOCK_MONOTONIC;
    case 4: return CLOCK_MONOTONIC_RAW;
    case 2: return CLOCK_PROCESS_CPUTIME_ID;
    case 3: return CLOCK_THREAD_CPUTIME_ID;
    default: return -1;
    }
}

/* ---------- Linux itimerval type ---------- */

/* Linux struct itimerval (same layout as macOS on LP64) */
typedef struct {
    linux_timeval_t it_interval;
    linux_timeval_t it_value;
} linux_itimerval_t;

/* ---------- Time/timer syscall handlers ---------- */

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

    struct timespec req = { .tv_sec = lreq.tv_sec, .tv_nsec = lreq.tv_nsec };
    struct timespec rem = {0};

    if (nanosleep(&req, &rem) < 0) {
        if (rem_gva) {
            linux_timespec_t lrem = { .tv_sec = rem.tv_sec, .tv_nsec = rem.tv_nsec };
            guest_write(g, rem_gva, &lrem, sizeof(lrem));
        }
        return linux_errno();
    }

    return 0;
}

int64_t sys_clock_nanosleep(guest_t *g, int clockid, int flags,
                            uint64_t req_gva, uint64_t rem_gva) {
    #define TIMER_ABSTIME 1

    linux_timespec_t lreq;
    if (guest_read(g, req_gva, &lreq, sizeof(lreq)) < 0)
        return -LINUX_EFAULT;

    int mac_clockid = translate_clockid(clockid);
    if (mac_clockid < 0) return -LINUX_EINVAL;

    struct timespec req;

    if (flags & TIMER_ABSTIME) {
        /* Absolute timeout: compute remaining time from clock - target */
        struct timespec now;
        if (clock_gettime(mac_clockid, &now) < 0)
            return linux_errno();

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
            guest_write(g, rem_gva, &lrem, sizeof(lrem));
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
    if (guest_read(g, new_gva, &lnew, sizeof(lnew)) < 0)
        return -LINUX_EFAULT;

    /* ITIMER_REAL (which=0) is emulated internally because macOS shares
     * alarm() and setitimer(ITIMER_REAL) as the same underlying timer,
     * and hl needs alarm() for its per-iteration vCPU timeout. */
    if (which == 0) {
        struct timeval val = { .tv_sec = (long)lnew.it_value.tv_sec,
                               .tv_usec = (int)lnew.it_value.tv_usec };
        struct timeval itv = { .tv_sec = (long)lnew.it_interval.tv_sec,
                               .tv_usec = (int)lnew.it_interval.tv_usec };
        struct timeval old_val, old_itv;
        signal_set_itimer(&val, &itv,
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

    /* ITIMER_VIRTUAL and ITIMER_PROF: forward to host */
    struct itimerval nval = {
        .it_interval = { .tv_sec = (long)lnew.it_interval.tv_sec,
                         .tv_usec = (int)lnew.it_interval.tv_usec },
        .it_value    = { .tv_sec = (long)lnew.it_value.tv_sec,
                         .tv_usec = (int)lnew.it_value.tv_usec },
    };

    struct itimerval oval;
    if (setitimer(which, &nval, old_gva ? &oval : NULL) < 0)
        return linux_errno();

    if (old_gva) {
        linux_itimerval_t lold = {
            .it_interval = { .tv_sec = oval.it_interval.tv_sec,
                             .tv_usec = oval.it_interval.tv_usec },
            .it_value    = { .tv_sec = oval.it_value.tv_sec,
                             .tv_usec = oval.it_value.tv_usec },
        };
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
