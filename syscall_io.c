/* syscall_io.c — I/O syscall handlers for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Read/write, ioctl, splice, sendfile, poll/select operations. All functions
 * are called from syscall_dispatch() in syscall.c.
 */
#include "syscall_io.h"
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
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/event.h>
#include <poll.h>
#include <termios.h>

/* ---------- Linux terminal struct types ---------- */

/* Linux struct winsize (same layout as macOS) */
typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} linux_winsize_t;

/* Linux struct termios (aarch64): c_iflag..c_lflag are uint32_t,
 * c_line is uint8_t, c_cc has 19 entries, then speed fields.
 * macOS termios layout differs, so we translate field by field. */
typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
    uint32_t c_ispeed;  /* input speed (not in POSIX, but Linux has it) */
    uint32_t c_ospeed;  /* output speed */
} linux_termios_t;

/* ---------- basic read/write ---------- */

int64_t sys_write(guest_t *g, int fd, uint64_t buf_gva, uint64_t count) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    ssize_t ret = write(host_fd, buf, count);
    if (ret < 0) {
        if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        return linux_errno();
    }
    return ret;
}

int64_t sys_read(guest_t *g, int fd, uint64_t buf_gva, uint64_t count) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    ssize_t ret = read(host_fd, buf, count);
    return ret < 0 ? linux_errno() : ret;
}

int64_t sys_pread64(guest_t *g, int fd, uint64_t buf_gva,
                    uint64_t count, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    ssize_t ret = pread(host_fd, buf, count, offset);
    return ret < 0 ? linux_errno() : ret;
}

int64_t sys_pwrite64(guest_t *g, int fd, uint64_t buf_gva,
                     uint64_t count, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    ssize_t ret = pwrite(host_fd, buf, count, offset);
    if (ret < 0) {
        if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        return linux_errno();
    }
    return ret;
}

int64_t sys_readv(guest_t *g, int fd, uint64_t iov_gva, int iovcnt) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -LINUX_EINVAL;

    linux_iovec_t *guest_iov = guest_ptr(g, iov_gva);
    if (!guest_iov) return -LINUX_EFAULT;

    struct iovec *host_iov = alloca(iovcnt * sizeof(struct iovec));
    for (int i = 0; i < iovcnt; i++) {
        void *base = guest_ptr(g, guest_iov[i].iov_base);
        if (!base) return -LINUX_EFAULT;
        host_iov[i].iov_base = base;
        host_iov[i].iov_len = guest_iov[i].iov_len;
    }

    ssize_t ret = readv(host_fd, host_iov, iovcnt);
    return ret < 0 ? linux_errno() : ret;
}

int64_t sys_writev(guest_t *g, int fd, uint64_t iov_gva, int iovcnt) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -LINUX_EINVAL;

    /* Read iovec array from guest */
    linux_iovec_t *guest_iov = guest_ptr(g, iov_gva);
    if (!guest_iov) return -LINUX_EFAULT;

    /* Build host iovec array */
    struct iovec *host_iov = alloca(iovcnt * sizeof(struct iovec));

    for (int i = 0; i < iovcnt; i++) {
        void *base = guest_ptr(g, guest_iov[i].iov_base);
        if (!base) return -LINUX_EFAULT;
        host_iov[i].iov_base = base;
        host_iov[i].iov_len = guest_iov[i].iov_len;
    }

    ssize_t ret = writev(host_fd, host_iov, iovcnt);
    if (ret < 0) {
        if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        return linux_errno();
    }
    return ret;
}

/* ---------- terminal I/O ---------- */

int64_t sys_ioctl(guest_t *g, int fd, uint64_t request, uint64_t arg) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    switch (request) {
    case LINUX_TIOCGWINSZ: {
        /* Get terminal window size */
        struct winsize ws;
        if (ioctl(host_fd, TIOCGWINSZ, &ws) < 0)
            return -LINUX_ENOTTY;
        linux_winsize_t lws = {
            .ws_row = ws.ws_row,
            .ws_col = ws.ws_col,
            .ws_xpixel = ws.ws_xpixel,
            .ws_ypixel = ws.ws_ypixel,
        };
        if (guest_write(g, arg, &lws, sizeof(lws)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    case LINUX_TCGETS: {
        /* Get terminal attributes */
        struct termios t;
        if (tcgetattr(host_fd, &t) < 0)
            return -LINUX_ENOTTY;
        linux_termios_t lt = {0};
        lt.c_iflag = (uint32_t)t.c_iflag;
        lt.c_oflag = (uint32_t)t.c_oflag;
        lt.c_cflag = (uint32_t)t.c_cflag;
        lt.c_lflag = (uint32_t)t.c_lflag;
        /* Copy cc values (min of both sizes) */
        for (int i = 0; i < 19 && i < NCCS; i++)
            lt.c_cc[i] = t.c_cc[i];
        lt.c_ispeed = (uint32_t)cfgetispeed(&t);
        lt.c_ospeed = (uint32_t)cfgetospeed(&t);
        if (guest_write(g, arg, &lt, sizeof(lt)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    case LINUX_TCSETS:
    case LINUX_TCSETSW:
    case LINUX_TCSETSF: {
        /* Set terminal attributes (TCSETS=immediate, TCSETSW=drain, TCSETSF=flush) */
        linux_termios_t lt;
        if (guest_read(g, arg, &lt, sizeof(lt)) < 0)
            return -LINUX_EFAULT;
        struct termios t;
        tcgetattr(host_fd, &t); /* Get current as base */
        t.c_iflag = lt.c_iflag;
        t.c_oflag = lt.c_oflag;
        t.c_cflag = lt.c_cflag;
        t.c_lflag = lt.c_lflag;
        for (int i = 0; i < 19 && i < NCCS; i++)
            t.c_cc[i] = lt.c_cc[i];
        cfsetispeed(&t, lt.c_ispeed);
        cfsetospeed(&t, lt.c_ospeed);
        int action = (request == LINUX_TCSETSF) ? TCSAFLUSH
                   : (request == LINUX_TCSETSW) ? TCSADRAIN
                   : TCSANOW;
        if (tcsetattr(host_fd, action, &t) < 0)
            return linux_errno();
        return 0;
    }

    case LINUX_TIOCGPGRP: {
        /* Get foreground process group */
        pid_t pgrp = tcgetpgrp(host_fd);
        if (pgrp < 0) return -LINUX_ENOTTY;
        int32_t val = (int32_t)proc_get_pid();
        if (guest_write(g, arg, &val, sizeof(val)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    case LINUX_TIOCSPGRP: {
        /* Set foreground process group — stub (single-process model) */
        return 0;
    }

    case LINUX_TIOCSCTTY: {
        /* Acquire controlling terminal — stub */
        return 0;
    }

    case LINUX_TIOCNOTTY: {
        /* Release controlling terminal — stub */
        return 0;
    }

    case LINUX_FIONREAD: {
        /* Get bytes available for reading */
        int avail = 0;
        if (ioctl(host_fd, FIONREAD, &avail) < 0)
            return linux_errno();
        int32_t val = (int32_t)avail;
        if (guest_write(g, arg, &val, sizeof(val)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    case LINUX_TIOCGSID: {
        /* Get session ID — return our PID (single-process model) */
        int32_t val = (int32_t)proc_get_pid();
        if (guest_write(g, arg, &val, sizeof(val)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    default:
        return -LINUX_ENOTTY;
    }
}

/* ---------- file space/copy ---------- */

int64_t sys_fallocate(int fd, int mode, int64_t offset, int64_t len) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* mode 0 = basic allocation → ftruncate fallback.
     * Other modes (FALLOC_FL_PUNCH_HOLE etc.) not supported. */
    if (mode != 0) return -LINUX_EOPNOTSUPP;

    struct stat st;
    if (fstat(host_fd, &st) < 0) return linux_errno();

    /* Extend file if needed (ftruncate only extends, doesn't shrink) */
    int64_t new_size = offset + len;
    if (new_size > st.st_size) {
        if (ftruncate(host_fd, new_size) < 0)
            return linux_errno();
    }
    return 0;
}

int64_t sys_sendfile(guest_t *g, int out_fd, int in_fd,
                     uint64_t offset_gva, uint64_t count) {
    int host_out = fd_to_host(out_fd);
    int host_in = fd_to_host(in_fd);
    if (host_out < 0 || host_in < 0) return -LINUX_EBADF;

    /* macOS sendfile() requires a socket destination, so we emulate
     * with pread/write loop for general file-to-file copies. */
    int64_t offset = -1;
    if (offset_gva != 0) {
        if (guest_read(g, offset_gva, &offset, 8) < 0)
            return -LINUX_EFAULT;
    }

    char buf[65536];
    size_t total = 0;
    size_t remaining = count;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        ssize_t nr;
        if (offset >= 0) {
            nr = pread(host_in, buf, chunk, offset);
        } else {
            nr = read(host_in, buf, chunk);
        }
        if (nr <= 0) break;

        ssize_t nw = write(host_out, buf, nr);
        if (nw < 0) return total > 0 ? (int64_t)total : linux_errno();

        total += nw;
        remaining -= nw;
        if (offset >= 0) offset += nw;
        if (nw < nr) break;  /* Short write */
    }

    /* Write back updated offset */
    if (offset_gva != 0) {
        guest_write(g, offset_gva, &offset, 8);
    }

    return (int64_t)total;
}

int64_t sys_copy_file_range(guest_t *g, int fd_in, uint64_t off_in_gva,
                            int fd_out, uint64_t off_out_gva,
                            uint64_t len, unsigned int flags) {
    (void)flags;
    int host_in = fd_to_host(fd_in);
    int host_out = fd_to_host(fd_out);
    if (host_in < 0 || host_out < 0) return -LINUX_EBADF;

    /* Read optional offsets from guest memory */
    int64_t off_in = -1, off_out = -1;
    if (off_in_gva != 0) {
        if (guest_read(g, off_in_gva, &off_in, 8) < 0)
            return -LINUX_EFAULT;
    }
    if (off_out_gva != 0) {
        if (guest_read(g, off_out_gva, &off_out, 8) < 0)
            return -LINUX_EFAULT;
    }

    /* Emulate with pread/pwrite loop */
    char buf[65536];
    size_t total = 0;
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        ssize_t nr;
        if (off_in >= 0) {
            nr = pread(host_in, buf, chunk, off_in);
        } else {
            nr = read(host_in, buf, chunk);
        }
        if (nr <= 0) break;

        ssize_t nw;
        if (off_out >= 0) {
            nw = pwrite(host_out, buf, nr, off_out);
        } else {
            nw = write(host_out, buf, nr);
        }
        if (nw < 0) return total > 0 ? (int64_t)total : linux_errno();

        total += nw;
        remaining -= nw;
        if (off_in >= 0) off_in += nw;
        if (off_out >= 0) off_out += nw;
        if (nw < nr) break;
    }

    /* Write back updated offsets */
    if (off_in_gva != 0) guest_write(g, off_in_gva, &off_in, 8);
    if (off_out_gva != 0) guest_write(g, off_out_gva, &off_out, 8);

    return (int64_t)total;
}

/* ---------- splice/tee ---------- */

/* splice: emulate by reading from in_fd and writing to out_fd */
int64_t sys_splice(guest_t *g, int fd_in, uint64_t off_in_gva,
                   int fd_out, uint64_t off_out_gva,
                   size_t len, unsigned int flags) {
    (void)flags;
    int host_in = fd_to_host(fd_in);
    int host_out = fd_to_host(fd_out);
    if (host_in < 0 || host_out < 0) return -LINUX_EBADF;

    /* Handle offsets */
    int64_t off_in = -1, off_out = -1;
    if (off_in_gva) {
        if (guest_read(g, off_in_gva, &off_in, 8) < 0)
            return -LINUX_EFAULT;
    }
    if (off_out_gva) {
        if (guest_read(g, off_out_gva, &off_out, 8) < 0)
            return -LINUX_EFAULT;
    }

    /* Emulate with read/write loop using a host-side buffer */
    size_t chunk = len > 65536 ? 65536 : len;
    uint8_t *buf = malloc(chunk);
    if (!buf) return -LINUX_ENOMEM;

    size_t total = 0;
    while (total < len) {
        size_t n = (len - total) > chunk ? chunk : (len - total);
        ssize_t r = (off_in >= 0) ? pread(host_in, buf, n, off_in)
                                  : read(host_in, buf, n);
        if (r <= 0) break;
        if (off_in >= 0) off_in += r;

        size_t written = 0;
        while (written < (size_t)r) {
            ssize_t w = (off_out >= 0)
                ? pwrite(host_out, buf + written, r - written, off_out)
                : write(host_out, buf + written, r - written);
            if (w <= 0) { free(buf); goto done; }
            written += w;
            if (off_out >= 0) off_out += w;
        }
        total += r;
    }

done:
    free(buf);

    /* Write back updated offsets */
    if (off_in_gva && off_in >= 0)
        guest_write(g, off_in_gva, &off_in, 8);
    if (off_out_gva && off_out >= 0)
        guest_write(g, off_out_gva, &off_out, 8);

    return total > 0 ? (int64_t)total : linux_errno();
}

/* vmsplice: emulate as writev to the pipe fd */
int64_t sys_vmsplice(guest_t *g, int fd, uint64_t iov_gva,
                     unsigned long nr_segs, unsigned int flags) {
    (void)flags;
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (nr_segs > 64) nr_segs = 64;

    size_t total = 0;
    for (unsigned long i = 0; i < nr_segs; i++) {
        linux_iovec_t liov;
        if (guest_read(g, iov_gva + i * sizeof(linux_iovec_t),
                       &liov, sizeof(liov)) < 0)
            return -LINUX_EFAULT;

        void *src = guest_ptr(g, liov.iov_base);
        if (!src) return -LINUX_EFAULT;

        ssize_t w = write(host_fd, src, liov.iov_len);
        if (w < 0) return total > 0 ? (int64_t)total : linux_errno();
        total += w;
        if ((size_t)w < liov.iov_len) break;
    }

    return (int64_t)total;
}

/* tee: copy data between two pipes without consuming it.
 * Full emulation would need MSG_PEEK on pipe — just return -EINVAL
 * since it's rarely used. */
int64_t sys_tee(int fd_in, int fd_out, size_t len, unsigned int flags) {
    (void)fd_in; (void)fd_out; (void)len; (void)flags;
    return -LINUX_EINVAL;
}

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
    (void)clockid; /* We use kqueue timers, ignore clock source */
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

    /* Return old value if requested */
    if (old_value_gva) {
        linux_itimerspec_t old = {0};
        if (timerfd_state[slot].armed) {
            old.it_interval_sec = timerfd_state[slot].interval_ns / 1000000000LL;
            old.it_interval_nsec = timerfd_state[slot].interval_ns % 1000000000LL;
            /* it_value is approximation — we don't track remaining time */
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
        timerfd_state[slot].expirations = 0;
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
        /* Remaining time is an approximation */
        its.it_value_sec = 0;
        its.it_value_nsec = 1; /* Non-zero to indicate armed */
    }
    guest_write(g, curr_value_gva, &its, sizeof(its));
    return 0;
}
