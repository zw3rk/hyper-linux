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
#include "guest.h"

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/event.h>
#include <poll.h>

/* ---------- polling/select ---------- */

int64_t sys_ppoll(guest_t *g, uint64_t fds_gva, uint32_t nfds,
                  uint64_t timeout_gva, uint64_t sigmask_gva) {
    (void)sigmask_gva;  /* Ignore signal mask (single-threaded) */

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

    /* Convert timeout */
    int timeout_ms = -1;  /* Infinite by default */
    if (timeout_gva != 0) {
        linux_timespec_t lts;
        if (guest_read(g, timeout_gva, &lts, sizeof(lts)) < 0)
            return -LINUX_EFAULT;
        timeout_ms = (int)(lts.tv_sec * 1000 + lts.tv_nsec / 1000000);
    }

    int ret = poll(host_fds, nfds, timeout_ms);
    if (ret < 0) return linux_errno();

    /* Write back revents to guest */
    for (uint32_t i = 0; i < nfds; i++) {
        guest_fds[i].revents = host_fds[i].revents;
    }
    if (nfds > 0) {
        guest_write(g, fds_gva, guest_fds, nfds * sizeof(linux_pollfd_t));
    }

    return ret;
}

int64_t sys_pselect6(guest_t *g, int nfds, uint64_t readfds_gva,
                     uint64_t writefds_gva, uint64_t exceptfds_gva,
                     uint64_t timeout_gva) {
    /* Simple implementation: only handle timeout-based sleeps and basic
     * fd monitoring. For most coreutils, pselect is used for timed waits. */
    if (nfds < 0 || nfds > FD_SETSIZE) return -LINUX_EINVAL;

    fd_set read_set, write_set, except_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&except_set);

    int max_host_fd = -1;

    /* Translate fd_sets from guest. Linux fd_set uses unsigned long bitmask. */
    if (readfds_gva || writefds_gva || exceptfds_gva) {
        /* For simplicity with small fd counts, read the bitmask and translate */
        uint64_t rbits[2] = {0}, wbits[2] = {0}, ebits[2] = {0};
        if (readfds_gva)
            guest_read(g, readfds_gva, rbits, ((nfds + 63) / 64) * 8);
        if (writefds_gva)
            guest_read(g, writefds_gva, wbits, ((nfds + 63) / 64) * 8);
        if (exceptfds_gva)
            guest_read(g, exceptfds_gva, ebits, ((nfds + 63) / 64) * 8);

        for (int i = 0; i < nfds; i++) {
            int host_fd = fd_to_host(i);
            if (host_fd < 0) continue;
            if (host_fd > max_host_fd) max_host_fd = host_fd;

            if (rbits[i / 64] & (1ULL << (i % 64)))
                FD_SET(host_fd, &read_set);
            if (wbits[i / 64] & (1ULL << (i % 64)))
                FD_SET(host_fd, &write_set);
            if (ebits[i / 64] & (1ULL << (i % 64)))
                FD_SET(host_fd, &except_set);
        }
    }

    struct timespec ts, *tsp = NULL;
    if (timeout_gva != 0) {
        linux_timespec_t lts;
        if (guest_read(g, timeout_gva, &lts, sizeof(lts)) < 0)
            return -LINUX_EFAULT;
        ts.tv_sec = lts.tv_sec;
        ts.tv_nsec = lts.tv_nsec;
        tsp = &ts;
    }

    int ret = pselect(max_host_fd + 1,
                      readfds_gva ? &read_set : NULL,
                      writefds_gva ? &write_set : NULL,
                      exceptfds_gva ? &except_set : NULL,
                      tsp, NULL);
    if (ret < 0) return linux_errno();

    /* Write back result fd_sets (zero then set bits for matching fds) */
    if (readfds_gva || writefds_gva || exceptfds_gva) {
        uint64_t rbits[2] = {0}, wbits[2] = {0}, ebits[2] = {0};
        for (int i = 0; i < nfds; i++) {
            int host_fd = fd_to_host(i);
            if (host_fd < 0) continue;
            if (readfds_gva && FD_ISSET(host_fd, &read_set))
                rbits[i / 64] |= (1ULL << (i % 64));
            if (writefds_gva && FD_ISSET(host_fd, &write_set))
                wbits[i / 64] |= (1ULL << (i % 64));
            if (exceptfds_gva && FD_ISSET(host_fd, &except_set))
                ebits[i / 64] |= (1ULL << (i % 64));
        }
        int bytes = ((nfds + 63) / 64) * 8;
        if (readfds_gva) guest_write(g, readfds_gva, rbits, bytes);
        if (writefds_gva) guest_write(g, writefds_gva, wbits, bytes);
        if (exceptfds_gva) guest_write(g, exceptfds_gva, ebits, bytes);
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

/* Per-fd epoll registration data (stores user data for kevent→epoll
 * result translation). Indexed by guest fd. */
static struct {
    uint32_t events;   /* Registered EPOLL* events mask */
    uint64_t data;     /* User data to return in epoll_wait */
    int      active;   /* 1 if registered in some epoll instance */
} epoll_regs[FD_TABLE_SIZE];

int64_t sys_epoll_create1(int flags) {
    int kq = kqueue();
    if (kq < 0) return linux_errno();

    if (flags & LINUX_EPOLL_CLOEXEC)
        fcntl(kq, F_SETFD, FD_CLOEXEC);

    int gfd = fd_alloc(FD_EPOLL, kq);
    if (gfd < 0) { close(kq); return -LINUX_EMFILE; }

    int lflags = 0;
    if (flags & LINUX_EPOLL_CLOEXEC) lflags |= LINUX_O_CLOEXEC;
    fd_table[gfd].linux_flags = lflags;

    return gfd;
}

int64_t sys_epoll_ctl(guest_t *g, int epfd, int op, int fd,
                       uint64_t event_gva) {
    int kq_fd = fd_to_host(epfd);
    if (kq_fd < 0) return -LINUX_EBADF;
    if (fd_table[epfd].type != FD_EPOLL) return -LINUX_EINVAL;

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    if (op == LINUX_EPOLL_CTL_DEL) {
        /* Remove all filters for this fd */
        struct kevent changes[2];
        int nchanges = 0;
        if (epoll_regs[fd].active) {
            if (epoll_regs[fd].events & LINUX_EPOLLIN) {
                EV_SET(&changes[nchanges], host_fd, EVFILT_READ,
                       EV_DELETE, 0, 0, NULL);
                nchanges++;
            }
            if (epoll_regs[fd].events & LINUX_EPOLLOUT) {
                EV_SET(&changes[nchanges], host_fd, EVFILT_WRITE,
                       EV_DELETE, 0, 0, NULL);
                nchanges++;
            }
            /* Ignore errors from EV_DELETE (fd might already be closed) */
            kevent(kq_fd, changes, nchanges, NULL, 0, NULL);
            epoll_regs[fd].active = 0;
        }
        return 0;
    }

    /* ADD or MOD: read the epoll_event from guest */
    linux_epoll_event_t ev;
    if (guest_read(g, event_gva, &ev, sizeof(ev)) < 0)
        return -LINUX_EFAULT;

    /* For MOD, remove old registrations first */
    if (op == LINUX_EPOLL_CTL_MOD && epoll_regs[fd].active) {
        struct kevent del[2];
        int ndel = 0;
        if (epoll_regs[fd].events & LINUX_EPOLLIN) {
            EV_SET(&del[ndel], host_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            ndel++;
        }
        if (epoll_regs[fd].events & LINUX_EPOLLOUT) {
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

    /* Store registration data */
    epoll_regs[fd].events = ev.events;
    epoll_regs[fd].data = ev.data;
    epoll_regs[fd].active = 1;

    return 0;
}

int64_t sys_epoll_pwait(guest_t *g, int epfd, uint64_t events_gva,
                         int maxevents, int timeout_ms,
                         uint64_t sigmask_gva) {
    (void)sigmask_gva; /* Signal mask not implemented (single-threaded) */

    int kq_fd = fd_to_host(epfd);
    if (kq_fd < 0) return -LINUX_EBADF;
    if (fd_table[epfd].type != FD_EPOLL) return -LINUX_EINVAL;
    if (maxevents <= 0) return -LINUX_EINVAL;

    /* Convert timeout */
    struct timespec ts, *tsp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    /* Collect kqueue events */
    int cap = maxevents * 2; /* Each epoll fd can produce 2 kevents */
    if (cap > 256) cap = 256;
    struct kevent kevents[256];

    int nready = kevent(kq_fd, NULL, 0, kevents, cap, tsp);
    if (nready < 0) return linux_errno();

    /* Merge kevent results into epoll_event results. Multiple kevents
     * for the same fd (READ + WRITE) merge into one epoll_event. */
    linux_epoll_event_t out[256];
    int nout = 0;

    for (int i = 0; i < nready && nout < maxevents; i++) {
        int gfd = (int)(uintptr_t)kevents[i].udata;
        if (gfd < 0 || gfd >= FD_TABLE_SIZE || !epoll_regs[gfd].active)
            continue;

        /* Check if we already have an entry for this guest fd */
        int merged = 0;
        for (int j = 0; j < nout; j++) {
            if (out[j].data == epoll_regs[gfd].data) {
                /* Same fd — merge events */
                if (kevents[i].filter == EVFILT_READ)
                    out[j].events |= LINUX_EPOLLIN;
                if (kevents[i].filter == EVFILT_WRITE)
                    out[j].events |= LINUX_EPOLLOUT;
                if (kevents[i].flags & EV_EOF)
                    out[j].events |= LINUX_EPOLLHUP;
                if (kevents[i].flags & EV_ERROR)
                    out[j].events |= LINUX_EPOLLERR;
                merged = 1;
                break;
            }
        }

        if (!merged) {
            out[nout].events = 0;
            out[nout]._pad = 0;
            out[nout].data = epoll_regs[gfd].data;
            if (kevents[i].filter == EVFILT_READ)
                out[nout].events |= LINUX_EPOLLIN;
            if (kevents[i].filter == EVFILT_WRITE)
                out[nout].events |= LINUX_EPOLLOUT;
            if (kevents[i].flags & EV_EOF)
                out[nout].events |= LINUX_EPOLLHUP;
            if (kevents[i].flags & EV_ERROR)
                out[nout].events |= LINUX_EPOLLERR;
            nout++;
        }
    }

    /* Write results to guest */
    if (nout > 0)
        guest_write(g, events_gva, out, nout * sizeof(linux_epoll_event_t));

    return nout;
}
