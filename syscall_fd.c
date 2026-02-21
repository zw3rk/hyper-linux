/* syscall_fd.c — Special FD types (eventfd, signalfd, timerfd) for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulates Linux eventfd (pipe+counter), signalfd (synthetic signal reads),
 * and timerfd (kqueue EVFILT_TIMER). Each provides special read/write/close
 * semantics dispatched from sys_read/sys_write/sys_close.
 */
#include "syscall_fd.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "syscall_proc.h"
#include "syscall_signal.h"
#include "guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/event.h>

/* ================================================================
 * timerfd emulation via kqueue EVFILT_TIMER
 *
 * Each timerfd_create() creates a kqueue + timer registration.
 * Reads from the timerfd return an 8-byte counter of expirations.
 * ================================================================ */

/* Linux itimerspec for timerfd_settime/gettime */
typedef struct {
    int64_t it_interval_sec;
    int64_t it_interval_nsec;
    int64_t it_value_sec;
    int64_t it_value_nsec;
} linux_itimerspec_t;

/* Per-timerfd state (stored alongside the FD_TIMERFD entry) */
#define TIMERFD_MAX 32
static struct {
    int      guest_fd;      /* Guest fd (-1 if unused) */
    int      kq_fd;         /* kqueue fd for this timer */
    uint64_t expirations;   /* Accumulated expiration count */
    int64_t  interval_ns;   /* Repeat interval (0 = one-shot) */
    int64_t  initial_ns;    /* Initial value at arm time (for gettime) */
    int64_t  arm_time_ns;   /* CLOCK_MONOTONIC time when timer was armed */
    int      clockid;       /* Linux clock ID (CLOCK_REALTIME=0, CLOCK_MONOTONIC=1) */
    int      armed;         /* 1 if timer is running */
} timerfd_state[TIMERFD_MAX];

static void timerfd_init_once(void) {
    static int inited = 0;
    if (!inited) {
        for (int i = 0; i < TIMERFD_MAX; i++)
            timerfd_state[i].guest_fd = -1;
        inited = 1;
    }
}

static int timerfd_find(int guest_fd) {
    for (int i = 0; i < TIMERFD_MAX; i++)
        if (timerfd_state[i].guest_fd == guest_fd) return i;
    return -1;
}

static int timerfd_alloc(void) {
    for (int i = 0; i < TIMERFD_MAX; i++)
        if (timerfd_state[i].guest_fd == -1) return i;
    return -1;
}

int64_t sys_timerfd_create(int clockid, int flags) {
    timerfd_init_once();

    int kq = kqueue();
    if (kq < 0) return linux_errno();

    if (flags & LINUX_O_CLOEXEC)
        fcntl(kq, F_SETFD, FD_CLOEXEC);
    if (flags & 0x800 /* TFD_NONBLOCK = SOCK_NONBLOCK */) {
        int fl = fcntl(kq, F_GETFL);
        if (fl >= 0) fcntl(kq, F_SETFL, fl | O_NONBLOCK);
    }

    int gfd = fd_alloc(FD_TIMERFD, kq);
    if (gfd < 0) { close(kq); return -LINUX_EMFILE; }

    int slot = timerfd_alloc();
    if (slot < 0) { close(kq); return -LINUX_ENOMEM; }

    timerfd_state[slot].guest_fd = gfd;
    timerfd_state[slot].kq_fd = kq;
    timerfd_state[slot].expirations = 0;
    timerfd_state[slot].interval_ns = 0;
    timerfd_state[slot].initial_ns = 0;
    timerfd_state[slot].arm_time_ns = 0;
    timerfd_state[slot].clockid = clockid;
    timerfd_state[slot].armed = 0;

    fd_table[gfd].linux_flags = (flags & LINUX_O_CLOEXEC) ? LINUX_O_CLOEXEC : 0;
    return gfd;
}

int64_t sys_timerfd_settime(guest_t *g, int fd, int flags,
                             uint64_t new_value_gva,
                             uint64_t old_value_gva) {
    (void)flags; /* TFD_TIMER_ABSTIME not supported */

    int slot = timerfd_find(fd);
    if (slot < 0) return -LINUX_EBADF;

    linux_itimerspec_t its;
    if (guest_read(g, new_value_gva, &its, sizeof(its)) < 0)
        return -LINUX_EFAULT;

    /* Return old value if requested (compute remaining time) */
    if (old_value_gva) {
        linux_itimerspec_t old = {0};
        if (timerfd_state[slot].armed) {
            old.it_interval_sec = timerfd_state[slot].interval_ns / 1000000000LL;
            old.it_interval_nsec = timerfd_state[slot].interval_ns % 1000000000LL;

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t now_ns = now.tv_sec * 1000000000LL + now.tv_nsec;
            int64_t elapsed = now_ns - timerfd_state[slot].arm_time_ns;
            int64_t remaining = timerfd_state[slot].initial_ns - elapsed;
            if (remaining > 0) {
                old.it_value_sec = remaining / 1000000000LL;
                old.it_value_nsec = remaining % 1000000000LL;
            }
        }
        guest_write(g, old_value_gva, &old, sizeof(old));
    }

    int kq = timerfd_state[slot].kq_fd;
    int64_t value_ms = its.it_value_sec * 1000 + its.it_value_nsec / 1000000;
    int64_t interval_ns = its.it_interval_sec * 1000000000LL + its.it_interval_nsec;

    if (value_ms == 0 && its.it_value_nsec == 0) {
        /* Disarm timer */
        struct kevent kev;
        EV_SET(&kev, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
        kevent(kq, &kev, 1, NULL, 0, NULL); /* Ignore error if not armed */
        timerfd_state[slot].armed = 0;
        timerfd_state[slot].expirations = 0;
    } else {
        /* Arm timer */
        struct kevent kev;
        uint16_t kflags = EV_ADD | EV_ENABLE;
        if (interval_ns == 0) kflags |= EV_ONESHOT;
        EV_SET(&kev, 1, EVFILT_TIMER, kflags, NOTE_USECONDS,
               value_ms * 1000, NULL);
        if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0)
            return linux_errno();
        timerfd_state[slot].armed = 1;
        timerfd_state[slot].interval_ns = interval_ns;
        timerfd_state[slot].initial_ns = its.it_value_sec * 1000000000LL + its.it_value_nsec;
        timerfd_state[slot].expirations = 0;

        /* Record arm time for gettime remaining-time calculation */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        timerfd_state[slot].arm_time_ns = now.tv_sec * 1000000000LL + now.tv_nsec;
    }

    return 0;
}

int64_t sys_timerfd_gettime(guest_t *g, int fd, uint64_t curr_value_gva) {
    int slot = timerfd_find(fd);
    if (slot < 0) return -LINUX_EBADF;

    linux_itimerspec_t its = {0};
    if (timerfd_state[slot].armed) {
        its.it_interval_sec = timerfd_state[slot].interval_ns / 1000000000LL;
        its.it_interval_nsec = timerfd_state[slot].interval_ns % 1000000000LL;

        /* Compute actual remaining time from arm time + initial value */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t now_ns = now.tv_sec * 1000000000LL + now.tv_nsec;
        int64_t elapsed = now_ns - timerfd_state[slot].arm_time_ns;
        int64_t remaining;

        if (timerfd_state[slot].interval_ns > 0) {
            /* Repeating timer: remaining = interval - (elapsed % interval) */
            int64_t total = timerfd_state[slot].initial_ns;
            if (elapsed >= total) {
                int64_t since_first = elapsed - total;
                remaining = timerfd_state[slot].interval_ns
                          - (since_first % timerfd_state[slot].interval_ns);
            } else {
                remaining = total - elapsed;
            }
        } else {
            /* One-shot: remaining = initial - elapsed */
            remaining = timerfd_state[slot].initial_ns - elapsed;
        }

        if (remaining <= 0) {
            /* Timer already expired (one-shot) */
            its.it_value_sec = 0;
            its.it_value_nsec = 0;
        } else {
            its.it_value_sec = remaining / 1000000000LL;
            its.it_value_nsec = remaining % 1000000000LL;
        }
    }
    guest_write(g, curr_value_gva, &its, sizeof(its));
    return 0;
}

/* Read from timerfd: collect pending timer events from the kqueue,
 * return accumulated expiration count as uint64_t. Resets count to 0
 * after read (Linux timerfd semantics). */
int64_t timerfd_read(int guest_fd, guest_t *g, uint64_t buf_gva, uint64_t count) {
    if (count < 8) return -LINUX_EINVAL;

    int slot = timerfd_find(guest_fd);
    if (slot < 0) return -LINUX_EBADF;

    int kq = timerfd_state[slot].kq_fd;

    /* Collect pending timer events via kevent(). The data field contains
     * the number of times the timer fired since the last kevent() call. */
    struct kevent kev;
    struct timespec ts_zero = {0, 0};
    int nev = kevent(kq, NULL, 0, &kev, 1, &ts_zero);
    if (nev > 0) {
        uint64_t fires = (uint64_t)kev.data;
        if (fires == 0) fires = 1; /* At least one expiration */
        timerfd_state[slot].expirations += fires;
    }

    if (timerfd_state[slot].expirations == 0) {
        /* No events yet — check if non-blocking */
        int fl = fcntl(kq, F_GETFL);
        if (fl >= 0 && (fl & O_NONBLOCK))
            return -LINUX_EAGAIN;

        /* Blocking: wait for the timer to fire */
        nev = kevent(kq, NULL, 0, &kev, 1, NULL);
        if (nev > 0) {
            uint64_t fires = (uint64_t)kev.data;
            if (fires == 0) fires = 1;
            timerfd_state[slot].expirations += fires;
        }
        if (timerfd_state[slot].expirations == 0)
            return -LINUX_EAGAIN;
    }

    uint64_t val = timerfd_state[slot].expirations;
    timerfd_state[slot].expirations = 0;

    if (guest_write(g, buf_gva, &val, 8) < 0)
        return -LINUX_EFAULT;

    return 8;
}

/* Clean up timerfd state when guest closes the fd. */
void timerfd_close(int guest_fd) {
    int slot = timerfd_find(guest_fd);
    if (slot < 0) return;
    /* kq_fd is closed by sys_close() as host_fd */
    timerfd_state[slot].guest_fd = -1;
    timerfd_state[slot].expirations = 0;
    timerfd_state[slot].armed = 0;
}

/* ================================================================
 * eventfd emulation via pipe + counter
 *
 * Linux eventfd is a semaphore-like fd: writes add to a uint64 counter,
 * reads return the counter and reset it to zero. EFD_SEMAPHORE mode
 * returns 1 per read and decrements. We emulate using a self-pipe
 * for poll/epoll compatibility plus a separate counter.
 * ================================================================ */

/* Linux eventfd flags */
#define LINUX_EFD_CLOEXEC    0x80000  /* Same as O_CLOEXEC on aarch64 */
#define LINUX_EFD_NONBLOCK   0x800    /* Same as O_NONBLOCK */
#define LINUX_EFD_SEMAPHORE  1

/* Per-eventfd state */
#define EVENTFD_MAX 32
static struct {
    int      guest_fd;     /* Guest fd (-1 if unused) */
    int      pipe_rd;      /* Read end of self-pipe (for poll/epoll readiness) */
    int      pipe_wr;      /* Write end of self-pipe */
    uint64_t counter;      /* Accumulated event counter */
    int      semaphore;    /* EFD_SEMAPHORE mode */
    int      nonblock;     /* O_NONBLOCK */
} eventfd_state[EVENTFD_MAX];

static void eventfd_init_once(void) {
    static int inited = 0;
    if (!inited) {
        for (int i = 0; i < EVENTFD_MAX; i++)
            eventfd_state[i].guest_fd = -1;
        inited = 1;
    }
}

static int eventfd_find(int guest_fd) {
    for (int i = 0; i < EVENTFD_MAX; i++)
        if (eventfd_state[i].guest_fd == guest_fd) return i;
    return -1;
}

static int eventfd_slot_alloc(void) {
    for (int i = 0; i < EVENTFD_MAX; i++)
        if (eventfd_state[i].guest_fd == -1) return i;
    return -1;
}

int64_t sys_eventfd2(unsigned int initval, int flags) {
    eventfd_init_once();

    /* Create self-pipe for poll/epoll readiness signaling */
    int pipefd[2];
    if (pipe(pipefd) < 0) return linux_errno();

    /* Make both ends non-blocking to avoid blocking on pipe I/O */
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    if (flags & LINUX_EFD_CLOEXEC) {
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    }

    /* Allocate guest fd — use read end as the host fd so epoll/poll sees it */
    int gfd = fd_alloc(FD_EVENTFD, pipefd[0]);
    if (gfd < 0) { close(pipefd[0]); close(pipefd[1]); return -LINUX_EMFILE; }

    int slot = eventfd_slot_alloc();
    if (slot < 0) { close(pipefd[0]); close(pipefd[1]); return -LINUX_ENOMEM; }

    eventfd_state[slot].guest_fd = gfd;
    eventfd_state[slot].pipe_rd = pipefd[0];
    eventfd_state[slot].pipe_wr = pipefd[1];
    eventfd_state[slot].counter = (uint64_t)initval;
    eventfd_state[slot].semaphore = (flags & LINUX_EFD_SEMAPHORE) ? 1 : 0;
    eventfd_state[slot].nonblock = (flags & LINUX_EFD_NONBLOCK) ? 1 : 0;

    fd_table[gfd].linux_flags = (flags & LINUX_EFD_CLOEXEC) ? LINUX_O_CLOEXEC : 0;

    /* If initial counter > 0, make the pipe readable so poll sees it */
    if (initval > 0) {
        uint8_t byte = 1;
        write(pipefd[1], &byte, 1);
    }

    return gfd;
}

/* Clean up eventfd state when guest closes the fd. */
void eventfd_close(int guest_fd) {
    int slot = eventfd_find(guest_fd);
    if (slot < 0) return;
    close(eventfd_state[slot].pipe_wr);
    /* pipe_rd is closed by sys_close() as host_fd */
    eventfd_state[slot].guest_fd = -1;
    eventfd_state[slot].counter = 0;
}

/* Read from eventfd: return 8-byte counter value, then reset to 0.
 * In EFD_SEMAPHORE mode, return 1 and decrement counter by 1. */
int64_t eventfd_read(int guest_fd, guest_t *g, uint64_t buf_gva, uint64_t count) {
    if (count < 8) return -LINUX_EINVAL;

    int slot = eventfd_find(guest_fd);
    if (slot < 0) return -LINUX_EBADF;

    if (eventfd_state[slot].counter == 0) {
        if (eventfd_state[slot].nonblock) return -LINUX_EAGAIN;
        /* Blocking mode: block on the pipe read end */
        uint8_t byte;
        ssize_t r = read(eventfd_state[slot].pipe_rd, &byte, 1);
        if (r < 0) return linux_errno();
        /* Counter was updated by the writer — re-check */
        if (eventfd_state[slot].counter == 0) return -LINUX_EAGAIN;
    }

    uint64_t val;
    if (eventfd_state[slot].semaphore) {
        val = 1;
        eventfd_state[slot].counter--;
    } else {
        val = eventfd_state[slot].counter;
        eventfd_state[slot].counter = 0;
    }

    /* Drain pipe readability if counter is now 0 */
    if (eventfd_state[slot].counter == 0) {
        uint8_t drain;
        while (read(eventfd_state[slot].pipe_rd, &drain, 1) > 0)
            ;
    }

    if (guest_write(g, buf_gva, &val, 8) < 0)
        return -LINUX_EFAULT;

    return 8;
}

/* Write to eventfd: add value to counter. Value must be uint64_t.
 * Maximum counter value is UINT64_MAX - 1. */
int64_t eventfd_write(int guest_fd, guest_t *g, uint64_t buf_gva, uint64_t count) {
    if (count < 8) return -LINUX_EINVAL;

    int slot = eventfd_find(guest_fd);
    if (slot < 0) return -LINUX_EBADF;

    uint64_t val;
    if (guest_read(g, buf_gva, &val, 8) < 0)
        return -LINUX_EFAULT;

    if (val == UINT64_MAX) return -LINUX_EINVAL;

    /* Check for counter overflow */
    if (eventfd_state[slot].counter > UINT64_MAX - 1 - val) {
        if (eventfd_state[slot].nonblock) return -LINUX_EAGAIN;
        /* In blocking mode we'd block; for now just clamp */
    }

    int was_zero = (eventfd_state[slot].counter == 0);
    eventfd_state[slot].counter += val;

    /* Signal readability via pipe if counter transitioned from 0 */
    if (was_zero && eventfd_state[slot].counter > 0) {
        uint8_t byte = 1;
        write(eventfd_state[slot].pipe_wr, &byte, 1);
    }

    return 8;
}

/* ================================================================
 * signalfd emulation
 *
 * Linux signalfd creates an fd from which pending signals can be read
 * as signalfd_siginfo structures (128 bytes each). We integrate with
 * the existing signal_state infrastructure — reads consume pending
 * signals that match the signalfd's mask.
 * ================================================================ */

/* Linux signalfd_siginfo structure (128 bytes) */
typedef struct {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t __pad2;
    int32_t  ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_arch;
    uint8_t  __pad[28];
} linux_signalfd_siginfo_t;

_Static_assert(sizeof(linux_signalfd_siginfo_t) == 128,
               "signalfd_siginfo must be 128 bytes");

/* Linux SFD_* flags (same values as O_* on aarch64) */
#define LINUX_SFD_CLOEXEC  0x80000
#define LINUX_SFD_NONBLOCK 0x800

/* Per-signalfd state */
#define SIGNALFD_MAX 16
static struct {
    int      guest_fd;   /* Guest fd (-1 if unused) */
    int      pipe_rd;    /* Read end for poll/epoll readiness */
    int      pipe_wr;    /* Write end for signaling */
    uint64_t mask;       /* Signal mask (bitmask of signals to accept) */
    int      nonblock;   /* O_NONBLOCK */
} signalfd_state[SIGNALFD_MAX];

static void signalfd_init_once(void) {
    static int inited = 0;
    if (!inited) {
        for (int i = 0; i < SIGNALFD_MAX; i++)
            signalfd_state[i].guest_fd = -1;
        inited = 1;
    }
}

static int signalfd_find(int guest_fd) {
    for (int i = 0; i < SIGNALFD_MAX; i++)
        if (signalfd_state[i].guest_fd == guest_fd) return i;
    return -1;
}

static int signalfd_slot_alloc(void) {
    for (int i = 0; i < SIGNALFD_MAX; i++)
        if (signalfd_state[i].guest_fd == -1) return i;
    return -1;
}

/* Clean up signalfd state when guest closes the fd. */
void signalfd_close(int guest_fd) {
    int slot = signalfd_find(guest_fd);
    if (slot < 0) return;
    close(signalfd_state[slot].pipe_wr);
    /* pipe_rd is closed by sys_close() as host_fd */
    signalfd_state[slot].guest_fd = -1;
    signalfd_state[slot].mask = 0;
}

int64_t sys_signalfd4(guest_t *g, int fd, uint64_t mask_gva,
                       uint64_t sigsetsize, int flags) {
    signalfd_init_once();

    /* Read the signal mask from guest memory */
    uint64_t mask = 0;
    if (sigsetsize < 8) return -LINUX_EINVAL;
    if (guest_read(g, mask_gva, &mask, 8) < 0)
        return -LINUX_EFAULT;

    /* If fd >= 0, update existing signalfd mask */
    if (fd >= 0) {
        int slot = signalfd_find(fd);
        if (slot < 0) return -LINUX_EINVAL;
        signalfd_state[slot].mask = mask;
        return fd;
    }

    /* Create new signalfd */
    int pipefd[2];
    if (pipe(pipefd) < 0) return linux_errno();

    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    if (flags & LINUX_SFD_CLOEXEC) {
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    }

    int gfd = fd_alloc(FD_SIGNALFD, pipefd[0]);
    if (gfd < 0) { close(pipefd[0]); close(pipefd[1]); return -LINUX_EMFILE; }

    int slot = signalfd_slot_alloc();
    if (slot < 0) { close(pipefd[0]); close(pipefd[1]); return -LINUX_ENOMEM; }

    signalfd_state[slot].guest_fd = gfd;
    signalfd_state[slot].pipe_rd = pipefd[0];
    signalfd_state[slot].pipe_wr = pipefd[1];
    signalfd_state[slot].mask = mask;
    signalfd_state[slot].nonblock = (flags & LINUX_SFD_NONBLOCK) ? 1 : 0;

    fd_table[gfd].linux_flags = (flags & LINUX_SFD_CLOEXEC) ? LINUX_O_CLOEXEC : 0;

    return gfd;
}

/* Read from signalfd: consume pending signals matching the mask.
 * Each signal produces one signalfd_siginfo (128 bytes).
 * Returns number of bytes read, or -EAGAIN if nothing pending. */
int64_t signalfd_read(int guest_fd, guest_t *g, uint64_t buf_gva, uint64_t count) {
    int slot = signalfd_find(guest_fd);
    if (slot < 0) return -LINUX_EBADF;

    uint64_t mask = signalfd_state[slot].mask;
    size_t max_signals = count / sizeof(linux_signalfd_siginfo_t);
    if (max_signals == 0) return -LINUX_EINVAL;

    const signal_state_t *sig = signal_get_state();
    /* Match pending signals against the signalfd mask. Do NOT filter by
     * sig->blocked — signalfd is specifically designed to read signals
     * that were blocked from normal delivery via sigprocmask(). */
    uint64_t deliverable = sig->pending & mask;

    if (deliverable == 0) {
        if (signalfd_state[slot].nonblock) return -LINUX_EAGAIN;
        /* In blocking mode, we'd block on the pipe. For single-threaded
         * guests, signals only arrive between syscalls, so return EAGAIN. */
        return -LINUX_EAGAIN;
    }

    size_t total = 0;
    for (int signum = 1; signum < LINUX_NSIG && total < max_signals; signum++) {
        uint64_t bit = 1ULL << (signum - 1);
        if (!(deliverable & bit)) continue;

        linux_signalfd_siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.ssi_signo = (uint32_t)signum;
        info.ssi_code = LINUX_SI_USER;
        info.ssi_pid = (uint32_t)proc_get_pid();
        info.ssi_uid = 1000;

        uint64_t off = total * sizeof(linux_signalfd_siginfo_t);
        if (guest_write(g, buf_gva + off, &info, sizeof(info)) < 0)
            return -LINUX_EFAULT;

        /* Consume the signal (clear pending bit) */
        signal_consume(signum);
        total++;
    }

    /* Drain pipe readability */
    uint8_t drain;
    while (read(signalfd_state[slot].pipe_rd, &drain, 1) > 0)
        ;

    return (int64_t)(total * sizeof(linux_signalfd_siginfo_t));
}

/* Notify signalfd pipes when a signal is queued. Called from
 * signal_queue() — writes a byte to make poll/epoll see readability. */
void signalfd_notify(int signum) {
    for (int i = 0; i < SIGNALFD_MAX; i++) {
        if (signalfd_state[i].guest_fd < 0) continue;
        uint64_t bit = 1ULL << (signum - 1);
        if (signalfd_state[i].mask & bit) {
            uint8_t byte = 1;
            write(signalfd_state[i].pipe_wr, &byte, 1);
        }
    }
}
