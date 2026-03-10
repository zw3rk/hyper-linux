/* syscall_poll.c — Poll/select/epoll syscall handlers for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * ppoll, pselect6, and epoll (emulated via macOS kqueue). All functions
 * are called from syscall_dispatch() in syscall.c.
 */
#include "syscall_poll.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "syscall_signal.h"
#include "syscall_proc.h"  /* exit_group_requested */
#include "guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <sys/event.h>
#include <poll.h>

/* Global wakeup pipe: write end signals exit_group/futex_interrupt to
 * threads blocked in host poll/select/kevent. The read end is added to
 * every blocking wait with infinite timeout. */
int wakeup_pipe_rd = -1;
static int wakeup_pipe_wr = -1;

void wakeup_pipe_init(void) {
    int pipefd[2];
    if (pipe(pipefd) == 0) {
        fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
        fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
        wakeup_pipe_rd = pipefd[0];
        wakeup_pipe_wr = pipefd[1];
    }
}

void wakeup_pipe_signal(void) {
    if (wakeup_pipe_wr >= 0) {
        uint8_t byte = 1;
        write(wakeup_pipe_wr, &byte, 1);
    }
}

/* ---------- polling/select ---------- */

int64_t sys_ppoll(guest_t *g, uint64_t fds_gva, uint32_t nfds,
                  uint64_t timeout_gva, uint64_t sigmask_gva) {
    if (nfds > 256) return -LINUX_EINVAL;

    /* Read pollfd array from guest (layout is identical to macOS) */
    linux_pollfd_t guest_fds[256];
    if (nfds > 0) {
        if (guest_read(g, fds_gva, guest_fds, nfds * sizeof(linux_pollfd_t)) < 0)
            return -LINUX_EFAULT;
    }

    /* Translate guest FDs to host FDs */
    struct pollfd host_fds[256];
    for (uint32_t i = 0; i < nfds; i++) {
        host_fds[i].fd = fd_to_host(guest_fds[i].fd);
        host_fds[i].events = guest_fds[i].events;
        host_fds[i].revents = 0;
    }

    /* Log fd types for shutdown diagnostics (verbose only) */
    if (hl_verbose && timeout_gva == 0) {
        fprintf(stderr, "hl: ppoll: nfds=%u infinite timeout, fds=[", nfds);
        for (uint32_t i = 0; i < nfds && i < 8; i++) {
            int gfd = guest_fds[i].fd;
            const char *type = "?";
            if (gfd >= 0 && gfd < FD_TABLE_SIZE) {
                switch (fd_table[gfd].type) {
                case FD_EVENTFD: type = "efd"; break;
                case FD_TIMERFD: type = "tfd"; break;
                case FD_EPOLL:   type = "epoll"; break;
                case FD_SIGNALFD: type = "sfd"; break;
                default: type = "fd"; break;
                }
            }
            fprintf(stderr, "%s%d(%s→%d)", i?",":"", gfd, type,
                    host_fds[i].fd);
        }
        fprintf(stderr, "]\n");
    }

    /* Convert timeout (compute in int64_t to avoid overflow, clamp to INT_MAX) */
    int timeout_ms = -1;  /* Infinite by default */
    if (timeout_gva != 0) {
        linux_timespec_t lts;
        if (guest_read(g, timeout_gva, &lts, sizeof(lts)) < 0)
            return -LINUX_EFAULT;
        /* Linux returns EINVAL for negative timeout values */
        if (lts.tv_sec < 0 || lts.tv_nsec < 0 || lts.tv_nsec >= 1000000000LL)
            return -LINUX_EINVAL;
        int64_t ms64 = lts.tv_sec * (int64_t)1000 + lts.tv_nsec / 1000000;
        timeout_ms = (ms64 > INT_MAX) ? INT_MAX : (int)ms64;
    }

    /* Atomically install signal mask for the duration of the poll */
    uint64_t saved_mask = 0;
    int mask_installed = 0;
    if (sigmask_gva != 0) {
        uint64_t new_mask;
        if (guest_read(g, sigmask_gva, &new_mask, sizeof(new_mask)) == 0) {
            saved_mask = signal_save_blocked();
            signal_set_blocked(new_mask);
            mask_installed = 1;
        }
    }

    /* For indefinite polls, add the wakeup pipe so exit_group can
     * interrupt threads blocked in host poll(). Without this, threads
     * in poll(timeout=-1) can't be interrupted by hv_vcpus_exit()
     * because they're not in hv_vcpu_run(). */
    int added_wakeup = 0;
    if (timeout_ms < 0 && wakeup_pipe_rd >= 0 && nfds < 256) {
        host_fds[nfds].fd = wakeup_pipe_rd;
        host_fds[nfds].events = POLLIN;
        host_fds[nfds].revents = 0;
        added_wakeup = 1;
    }

    extern _Atomic int futex_interrupt_requested;
    int ret;
    do {
        ret = poll(host_fds, nfds + added_wakeup, timeout_ms < 0 ? 200 : timeout_ms);

        /* Check for exit_group / futex_interrupt after waking */
        if (atomic_load(&exit_group_requested) ||
            atomic_load(&futex_interrupt_requested)) {
            ret = -1;
            errno = EINTR;
            break;
        }

        /* If we used a short timeout (200ms) on an infinite poll and
         * nothing happened, loop back. If the caller had a real timeout,
         * we only called poll once with that timeout, so break. */
    } while (ret == 0 && timeout_ms < 0);

    int saved_errno = errno;

    /* Drain the wakeup pipe if it fired, and subtract from count since
     * the wakeup pipe is not visible to the guest. */
    if (added_wakeup && (host_fds[nfds].revents & POLLIN)) {
        uint8_t drain;
        while (read(wakeup_pipe_rd, &drain, 1) > 0) ;
        if (ret > 0) ret--;
    }

    /* Restore original signal mask */
    if (mask_installed)
        signal_restore_blocked(saved_mask);

    if (ret < 0) { errno = saved_errno; return linux_errno(); }

    /* Write back revents to guest */
    for (uint32_t i = 0; i < nfds; i++) {
        guest_fds[i].revents = host_fds[i].revents;
    }
    if (nfds > 0) {
        if (guest_write(g, fds_gva, guest_fds, nfds * sizeof(linux_pollfd_t)) < 0)
            return -LINUX_EFAULT;
    }

    return ret;
}

int64_t sys_pselect6(guest_t *g, int nfds, uint64_t readfds_gva,
                     uint64_t writefds_gva, uint64_t exceptfds_gva,
                     uint64_t timeout_gva, uint64_t sigmask_gva) {
    /* pselect6 atomically sets the signal mask during the wait, then
     * restores it. The sixth argument is a pointer to a struct:
     *   { const sigset_t *ss; size_t ss_len; }   */
    if (nfds < 0 || nfds > FD_SETSIZE) return -LINUX_EINVAL;

    fd_set read_set, write_set, except_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&except_set);

    int max_host_fd = -1;

    /* Translate fd_sets from guest. Linux fd_set uses unsigned long bitmask.
     * FD_TABLE_SIZE=1024 → max 16 uint64_t words (128 bytes). */
    if (readfds_gva || writefds_gva || exceptfds_gva) {
        uint64_t rbits[FD_TABLE_SIZE / 64] = {0};
        uint64_t wbits[FD_TABLE_SIZE / 64] = {0};
        uint64_t ebits[FD_TABLE_SIZE / 64] = {0};
        size_t bitmask_bytes = ((nfds + 63) / 64) * 8;
        if (readfds_gva && guest_read(g, readfds_gva, rbits, bitmask_bytes) < 0)
            return -LINUX_EFAULT;
        if (writefds_gva && guest_read(g, writefds_gva, wbits, bitmask_bytes) < 0)
            return -LINUX_EFAULT;
        if (exceptfds_gva && guest_read(g, exceptfds_gva, ebits, bitmask_bytes) < 0)
            return -LINUX_EFAULT;

        for (int i = 0; i < nfds; i++) {
            int host_fd = fd_to_host(i);
            if (host_fd < 0) continue;
            /* Guard against host fds exceeding FD_SETSIZE (macOS stack
             * buffer overflow if the host fd number is >= FD_SETSIZE). */
            if (host_fd >= FD_SETSIZE) continue;
            if (host_fd > max_host_fd) max_host_fd = host_fd;

            if (rbits[i / 64] & (1ULL << (i % 64)))
                FD_SET(host_fd, &read_set);
            if (wbits[i / 64] & (1ULL << (i % 64)))
                FD_SET(host_fd, &write_set);
            if (ebits[i / 64] & (1ULL << (i % 64)))
                FD_SET(host_fd, &except_set);
        }
    }

    int has_timeout = (timeout_gva != 0);
    struct timespec ts;
    if (has_timeout) {
        linux_timespec_t lts;
        if (guest_read(g, timeout_gva, &lts, sizeof(lts)) < 0)
            return -LINUX_EFAULT;
        /* Linux returns EINVAL for negative or out-of-range timeout values */
        if (lts.tv_sec < 0 || lts.tv_nsec < 0 || lts.tv_nsec >= 1000000000LL)
            return -LINUX_EINVAL;
        ts.tv_sec = lts.tv_sec;
        ts.tv_nsec = lts.tv_nsec;
    }

    /* Apply signal mask atomically around the select.
     * Linux pselect6 arg6 points to { sigset_t *ss; size_t ss_len }.
     * Save the current blocked mask, apply the new one, do the select,
     * then restore the original mask. */
    uint64_t saved_blocked = 0;
    int mask_applied = 0;
    if (sigmask_gva) {
        struct { uint64_t ss; uint64_t ss_len; } ssarg;
        if (guest_read(g, sigmask_gva, &ssarg, sizeof(ssarg)) == 0
            && ssarg.ss != 0 && ssarg.ss_len == 8) {
            uint64_t new_mask;
            if (guest_read(g, ssarg.ss, &new_mask, 8) == 0) {
                saved_blocked = signal_save_blocked();
                signal_set_blocked(new_mask);
                mask_applied = 1;
            }
        }
    }

    /* For indefinite selects, add the wakeup pipe and use a short
     * timeout so exit_group can interrupt. */
    int added_wakeup = 0;
    if (!has_timeout && wakeup_pipe_rd >= 0) {
        FD_SET(wakeup_pipe_rd, &read_set);
        if (wakeup_pipe_rd > max_host_fd) max_host_fd = wakeup_pipe_rd;
        added_wakeup = 1;
    }

    extern _Atomic int futex_interrupt_requested;
    struct timespec poll_ts = { .tv_sec = 0, .tv_nsec = 200000000L }; /* 200ms */

    /* Save fd_sets — pselect modifies them in-place to indicate ready fds.
     * Without saving/restoring, the indefinite retry loop would operate on
     * corrupted (zeroed) fd_sets after a 200ms timeout iteration. */
    fd_set saved_read, saved_write, saved_except;
    if (!has_timeout) {
        saved_read   = read_set;
        saved_write  = write_set;
        saved_except = except_set;
    }

    int ret;
    do {
        if (!has_timeout) {
            read_set   = saved_read;
            write_set  = saved_write;
            except_set = saved_except;
        }

        ret = pselect(max_host_fd + 1,
                      readfds_gva ? &read_set : NULL,
                      writefds_gva ? &write_set : NULL,
                      exceptfds_gva ? &except_set : NULL,
                      has_timeout ? &ts : &poll_ts, NULL);

        if (atomic_load(&exit_group_requested) ||
            atomic_load(&futex_interrupt_requested)) {
            ret = -1;
            errno = EINTR;
            break;
        }
    } while (ret == 0 && !has_timeout);

    int save_errno = errno;

    /* Drain wakeup pipe if it fired, and subtract from count since
     * the wakeup pipe is not visible to the guest. */
    if (added_wakeup && FD_ISSET(wakeup_pipe_rd, &read_set)) {
        uint8_t drain;
        while (read(wakeup_pipe_rd, &drain, 1) > 0) ;
        FD_CLR(wakeup_pipe_rd, &read_set);
        if (ret > 0) ret--;
    }

    /* Restore original signal mask */
    if (mask_applied)
        signal_restore_blocked(saved_blocked);

    if (ret < 0) { errno = save_errno; return linux_errno(); }

    /* Write back result fd_sets (zero then set bits for matching fds) */
    if (readfds_gva || writefds_gva || exceptfds_gva) {
        uint64_t rbits[FD_TABLE_SIZE / 64] = {0};
        uint64_t wbits[FD_TABLE_SIZE / 64] = {0};
        uint64_t ebits[FD_TABLE_SIZE / 64] = {0};
        for (int i = 0; i < nfds; i++) {
            int host_fd = fd_to_host(i);
            if (host_fd < 0) continue;
            if (host_fd >= FD_SETSIZE) continue;  /* Must match setup loop guard */
            if (readfds_gva && FD_ISSET(host_fd, &read_set))
                rbits[i / 64] |= (1ULL << (i % 64));
            if (writefds_gva && FD_ISSET(host_fd, &write_set))
                wbits[i / 64] |= (1ULL << (i % 64));
            if (exceptfds_gva && FD_ISSET(host_fd, &except_set))
                ebits[i / 64] |= (1ULL << (i % 64));
        }
        int bytes = ((nfds + 63) / 64) * 8;
        if (readfds_gva && guest_write(g, readfds_gva, rbits, bytes) < 0)
            return -LINUX_EFAULT;
        if (writefds_gva && guest_write(g, writefds_gva, wbits, bytes) < 0)
            return -LINUX_EFAULT;
        if (exceptfds_gva && guest_write(g, exceptfds_gva, ebits, bytes) < 0)
            return -LINUX_EFAULT;
    }

    return ret;
}

/* ================================================================
 * epoll emulation via kqueue
 *
 * Linux epoll is emulated using macOS kqueue. Each epoll_create1()
 * creates a kqueue fd. epoll_ctl translates to kevent() calls.
 * epoll_pwait translates to kevent() with timeout.
 *
 * Limitations:
 *   - EPOLLEXCLUSIVE not supported (rare, for load balancing)
 *   - epoll_data is stored in a per-process registration table
 *     indexed by guest fd (one epoll registration per fd)
 * ================================================================ */

/* Linux EPOLL constants */
#define LINUX_EPOLLIN      0x001
#define LINUX_EPOLLOUT     0x004
#define LINUX_EPOLLERR     0x008
#define LINUX_EPOLLHUP     0x010
#define LINUX_EPOLLRDHUP   0x2000
#define LINUX_EPOLLET      (1U << 31)
#define LINUX_EPOLLONESHOT (1U << 30)

/* Linux epoll_ctl operations */
#define LINUX_EPOLL_CTL_ADD 1
#define LINUX_EPOLL_CTL_DEL 2
#define LINUX_EPOLL_CTL_MOD 3

/* Linux EPOLL_CLOEXEC = O_CLOEXEC = 0x80000 on aarch64 */
#define LINUX_EPOLL_CLOEXEC 0x80000

/* Linux epoll_event on aarch64 (NOT packed — 16 bytes with padding) */
typedef struct {
    uint32_t events;
    uint32_t _pad;
    uint64_t data;
} linux_epoll_event_t;

/* Per-fd registration entry within an epoll instance. */
typedef struct {
    uint32_t events;   /* Registered EPOLL* events mask */
    uint64_t data;     /* User data to return in epoll_wait */
    int      active;   /* 1 if registered in this instance */
} epoll_reg_t;

/* Per-epoll-instance data, stored in fd_table[epfd].dir. Each instance
 * has its own registration table so multiple epoll instances watching
 * the same FD don't overwrite each other's user data. */
typedef struct {
    epoll_reg_t regs[FD_TABLE_SIZE];
} epoll_instance_t;

int64_t sys_epoll_create1(int flags) {
    int kq = kqueue();
    if (kq < 0) return linux_errno();

    if (flags & LINUX_EPOLL_CLOEXEC)
        fcntl(kq, F_SETFD, FD_CLOEXEC);

    /* Allocate per-instance registration table */
    epoll_instance_t *inst = calloc(1, sizeof(epoll_instance_t));
    if (!inst) { close(kq); return -LINUX_ENOMEM; }

    int gfd = fd_alloc(FD_EPOLL, kq);
    if (gfd < 0) { free(inst); close(kq); return -LINUX_EMFILE; }

    fd_table[gfd].dir = inst;
    int lflags = 0;
    if (flags & LINUX_EPOLL_CLOEXEC) lflags |= LINUX_O_CLOEXEC;
    fd_table[gfd].linux_flags = lflags;

    return gfd;
}

int64_t sys_epoll_ctl(guest_t *g, int epfd, int op, int fd,
                       uint64_t event_gva) {
    /* Linux returns EINVAL when trying to add an epoll fd to itself */
    if (fd == epfd) return -LINUX_EINVAL;

    int kq_fd = fd_to_host(epfd);
    if (kq_fd < 0) return -LINUX_EBADF;
    if (fd_table[epfd].type != FD_EPOLL) return -LINUX_EINVAL;

    epoll_instance_t *inst = (epoll_instance_t *)fd_table[epfd].dir;
    if (!inst) return -LINUX_EINVAL;

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    epoll_reg_t *reg = &inst->regs[fd];

    if (op == LINUX_EPOLL_CTL_DEL) {
        /* Linux returns ENOENT when removing an unregistered fd */
        if (!reg->active) return -LINUX_ENOENT;

        /* Remove all filters for this fd. EPOLLRDHUP alone registers
         * EVFILT_READ (see ADD path), so check both EPOLLIN and EPOLLRDHUP. */
        struct kevent changes[2];
        int nchanges = 0;
        {
            if (reg->events & (LINUX_EPOLLIN | LINUX_EPOLLRDHUP)) {
                EV_SET(&changes[nchanges], host_fd, EVFILT_READ,
                       EV_DELETE, 0, 0, NULL);
                nchanges++;
            }
            if (reg->events & LINUX_EPOLLOUT) {
                EV_SET(&changes[nchanges], host_fd, EVFILT_WRITE,
                       EV_DELETE, 0, 0, NULL);
                nchanges++;
            }
            /* Ignore errors from EV_DELETE (fd might already be closed) */
            kevent(kq_fd, changes, nchanges, NULL, 0, NULL);
            reg->active = 0;
        }
        return 0;
    }

    /* Linux semantics: ADD fails with EEXIST if already registered;
     * MOD fails with ENOENT if not registered. */
    if (op == LINUX_EPOLL_CTL_ADD && reg->active) return -LINUX_EEXIST;
    if (op == LINUX_EPOLL_CTL_MOD && !reg->active) return -LINUX_ENOENT;

    /* ADD or MOD: read the epoll_event from guest */
    linux_epoll_event_t ev;
    if (guest_read(g, event_gva, &ev, sizeof(ev)) < 0)
        return -LINUX_EFAULT;

    /* For MOD, remove old registrations first.
     * EPOLLRDHUP alone registers EVFILT_READ (see ADD path), so check
     * both EPOLLIN and EPOLLRDHUP — same logic as CTL_DEL. */
    if (op == LINUX_EPOLL_CTL_MOD && reg->active) {
        struct kevent del[2];
        int ndel = 0;
        if (reg->events & (LINUX_EPOLLIN | LINUX_EPOLLRDHUP)) {
            EV_SET(&del[ndel], host_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            ndel++;
        }
        if (reg->events & LINUX_EPOLLOUT) {
            EV_SET(&del[ndel], host_fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            ndel++;
        }
        kevent(kq_fd, del, ndel, NULL, 0, NULL);
    }

    /* Build kevent changes */
    struct kevent changes[2];
    int nchanges = 0;
    uint16_t kflags = EV_ADD;
    if (ev.events & LINUX_EPOLLET) kflags |= EV_CLEAR;
    if (ev.events & LINUX_EPOLLONESHOT) kflags |= EV_ONESHOT;

    /* Use (void*)(uintptr_t)fd as udata to identify the guest fd */
    void *udata = (void *)(uintptr_t)fd;

    if (ev.events & (LINUX_EPOLLIN | LINUX_EPOLLRDHUP)) {
        EV_SET(&changes[nchanges], host_fd, EVFILT_READ,
               kflags, 0, 0, udata);
        nchanges++;
    }
    if (ev.events & LINUX_EPOLLOUT) {
        EV_SET(&changes[nchanges], host_fd, EVFILT_WRITE,
               kflags, 0, 0, udata);
        nchanges++;
    }

    if (nchanges > 0) {
        if (kevent(kq_fd, changes, nchanges, NULL, 0, NULL) < 0)
            return linux_errno();
    }

    /* Store registration data in per-instance table */
    reg->events = ev.events;
    reg->data = ev.data;
    reg->active = 1;

    return 0;
}

int64_t sys_epoll_pwait(guest_t *g, int epfd, uint64_t events_gva,
                         int maxevents, int timeout_ms,
                         uint64_t sigmask_gva) {
    int kq_fd = fd_to_host(epfd);
    if (kq_fd < 0) return -LINUX_EBADF;
    if (fd_table[epfd].type != FD_EPOLL) return -LINUX_EINVAL;
    if (maxevents <= 0) return -LINUX_EINVAL;

    epoll_instance_t *inst = (epoll_instance_t *)fd_table[epfd].dir;
    if (!inst) return -LINUX_EINVAL;

    /* Atomically install signal mask for the duration of the wait */
    uint64_t saved_mask = 0;
    int mask_installed = 0;
    if (sigmask_gva != 0) {
        uint64_t new_mask;
        if (guest_read(g, sigmask_gva, &new_mask, sizeof(new_mask)) == 0) {
            saved_mask = signal_save_blocked();
            signal_set_blocked(new_mask);
            mask_installed = 1;
        }
    }

    /* Convert timeout */
    int has_timeout = (timeout_ms >= 0);
    struct timespec ts;
    if (has_timeout) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
    }

    /* For indefinite waits, register the wakeup pipe with the kqueue
     * so exit_group can interrupt threads blocked in kevent(). */
    int added_wakeup = 0;
    if (!has_timeout && wakeup_pipe_rd >= 0) {
        struct kevent wake_ev;
        EV_SET(&wake_ev, wakeup_pipe_rd, EVFILT_READ, EV_ADD | EV_ONESHOT,
               0, 0, (void *)(uintptr_t)-1);
        kevent(kq_fd, &wake_ev, 1, NULL, 0, NULL);
        added_wakeup = 1;
    }

    /* Collect kqueue events. For indefinite waits, use a short timeout
     * and loop so exit_group can interrupt. Cap maxevents before multiply
     * to avoid signed integer overflow when maxevents is very large. */
    if (maxevents > 128) maxevents = 128;
    int cap = maxevents * 2; /* Each epoll fd can produce 2 kevents */
    if (cap > 256) cap = 256;
    /* Reserve one slot for the wakeup pipe event */
    if (added_wakeup && cap < 256) cap++;
    struct kevent kevents[256];

    extern _Atomic int futex_interrupt_requested;
    struct timespec poll_ts = { .tv_sec = 0, .tv_nsec = 200000000L }; /* 200ms */
    int nready;
    do {
        nready = kevent(kq_fd, NULL, 0, kevents, cap,
                        has_timeout ? &ts : &poll_ts);

        if (atomic_load(&exit_group_requested) ||
            atomic_load(&futex_interrupt_requested)) {
            nready = -1;
            errno = EINTR;
            break;
        }
    } while (nready == 0 && !has_timeout);

    int saved_errno = errno;

    /* Remove wakeup pipe registration and drain if it fired */
    if (added_wakeup) {
        struct kevent del_ev;
        EV_SET(&del_ev, wakeup_pipe_rd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(kq_fd, &del_ev, 1, NULL, 0, NULL);
        /* Drain the wakeup pipe */
        uint8_t drain;
        while (read(wakeup_pipe_rd, &drain, 1) > 0) ;
        /* Filter out wakeup pipe events from results */
        for (int i = 0; i < nready; i++) {
            if ((uintptr_t)kevents[i].udata == (uintptr_t)-1) {
                kevents[i] = kevents[nready - 1];
                nready--;
                i--;
            }
        }
    }

    /* Restore original signal mask after the blocking wait */
    if (mask_installed)
        signal_restore_blocked(saved_mask);

    if (nready < 0) { errno = saved_errno; return linux_errno(); }

    /* Merge kevent results into epoll_event results. Multiple kevents
     * for the same fd (READ + WRITE) merge into one epoll_event.
     * Use guest FD (not user data) as the merge key — two different
     * FDs could legitimately share the same epoll_data value. */
    linux_epoll_event_t out[256];
    int out_gfds[256];  /* Parallel array tracking which guest FD each entry is for */
    int nout = 0;

    for (int i = 0; i < nready && nout < maxevents; i++) {
        int gfd = (int)(uintptr_t)kevents[i].udata;
        if (gfd < 0 || gfd >= FD_TABLE_SIZE || !inst->regs[gfd].active)
            continue;

        epoll_reg_t *reg = &inst->regs[gfd];

        /* Check if we already have an entry for this guest fd */
        int merged = 0;
        for (int j = 0; j < nout; j++) {
            if (out_gfds[j] == gfd) {
                /* Same fd — merge events */
                if (kevents[i].filter == EVFILT_READ)
                    out[j].events |= LINUX_EPOLLIN;
                if (kevents[i].filter == EVFILT_WRITE)
                    out[j].events |= LINUX_EPOLLOUT;
                if (kevents[i].flags & EV_EOF) {
                    out[j].events |= LINUX_EPOLLHUP;
                    if (kevents[i].filter == EVFILT_READ &&
                        (reg->events & LINUX_EPOLLRDHUP))
                        out[j].events |= LINUX_EPOLLRDHUP;
                }
                if (kevents[i].flags & EV_ERROR)
                    out[j].events |= LINUX_EPOLLERR;
                merged = 1;
                break;
            }
        }

        if (!merged) {
            out_gfds[nout] = gfd;
            out[nout].events = 0;
            out[nout]._pad = 0;
            out[nout].data = reg->data;
            if (kevents[i].filter == EVFILT_READ)
                out[nout].events |= LINUX_EPOLLIN;
            if (kevents[i].filter == EVFILT_WRITE)
                out[nout].events |= LINUX_EPOLLOUT;
            if (kevents[i].flags & EV_EOF) {
                out[nout].events |= LINUX_EPOLLHUP;
                if (kevents[i].filter == EVFILT_READ &&
                    (reg->events & LINUX_EPOLLRDHUP))
                    out[nout].events |= LINUX_EPOLLRDHUP;
            }
            if (kevents[i].flags & EV_ERROR)
                out[nout].events |= LINUX_EPOLLERR;
            nout++;
        }
    }

    /* Clear registrations for EPOLLONESHOT FDs that fired.
     * kqueue already removed the event (EV_ONESHOT), but our table
     * must reflect that the registration is now disabled until re-armed. */
    for (int i = 0; i < nout; i++) {
        int gfd = out_gfds[i];
        if (gfd >= 0 && gfd < FD_TABLE_SIZE && inst->regs[gfd].active) {
            if (inst->regs[gfd].events & LINUX_EPOLLONESHOT)
                inst->regs[gfd].active = 0;
        }
    }

    /* Write results to guest */
    if (nout > 0) {
        if (guest_write(g, events_gva, out, nout * sizeof(linux_epoll_event_t)) < 0)
            return -LINUX_EFAULT;
    }

    return nout;
}
