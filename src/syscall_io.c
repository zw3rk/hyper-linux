/* syscall_io.c — Core I/O syscall handlers for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Read/write, ioctl, splice, sendfile, and copy_file_range operations.
 * All functions are called from syscall_dispatch() in syscall.c.
 *
 * Poll/select/epoll handlers are in syscall_poll.c.
 * Special FD types (eventfd, signalfd, timerfd) are in syscall_fd.c.
 */
#include "syscall_io.h"
#include "syscall_fd.h"
#include "syscall_inotify.h"
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
#include <sys/socket.h>
#include <pthread.h>
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
    if (fd >= 0 && fd < FD_TABLE_SIZE && fd_table[fd].type == FD_EVENTFD)
        return eventfd_write(fd, g, buf_gva, count);

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
    if (fd >= 0 && fd < FD_TABLE_SIZE) {
        if (fd_table[fd].type == FD_EVENTFD)
            return eventfd_read(fd, g, buf_gva, count);
        if (fd_table[fd].type == FD_SIGNALFD)
            return signalfd_read(fd, g, buf_gva, count);
        if (fd_table[fd].type == FD_TIMERFD)
            return timerfd_read(fd, g, buf_gva, count);
        if (fd_table[fd].type == FD_INOTIFY)
            return inotify_read(fd, g, buf_gva, count);
    }

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
        /* Validate the entire buffer is within guest memory */
        uint64_t iov_end = guest_iov[i].iov_base + guest_iov[i].iov_len;
        if (iov_end < guest_iov[i].iov_base) return -LINUX_EFAULT; /* overflow */
        if (guest_iov[i].iov_len > 0 && !guest_ptr(g, iov_end - 1))
            return -LINUX_EFAULT;
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
        /* Validate the entire buffer is within guest memory */
        uint64_t iov_end = guest_iov[i].iov_base + guest_iov[i].iov_len;
        if (iov_end < guest_iov[i].iov_base) return -LINUX_EFAULT; /* overflow */
        if (guest_iov[i].iov_len > 0 && !guest_ptr(g, iov_end - 1))
            return -LINUX_EFAULT;
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

/* Helper: build host iovec array from guest iovec array.
 * Returns 0 on success, -LINUX_EFAULT on bad guest pointer. */
static int64_t build_host_iov(guest_t *g, uint64_t iov_gva, int iovcnt,
                               struct iovec *host_iov) {
    linux_iovec_t *guest_iov = guest_ptr(g, iov_gva);
    if (!guest_iov) return -LINUX_EFAULT;
    for (int i = 0; i < iovcnt; i++) {
        void *base = guest_ptr(g, guest_iov[i].iov_base);
        if (!base) return -LINUX_EFAULT;
        uint64_t iov_end = guest_iov[i].iov_base + guest_iov[i].iov_len;
        if (iov_end < guest_iov[i].iov_base) return -LINUX_EFAULT;
        if (guest_iov[i].iov_len > 0 && !guest_ptr(g, iov_end - 1))
            return -LINUX_EFAULT;
        host_iov[i].iov_base = base;
        host_iov[i].iov_len = guest_iov[i].iov_len;
    }
    return 0;
}

int64_t sys_preadv(guest_t *g, int fd, uint64_t iov_gva,
                   int iovcnt, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -LINUX_EINVAL;

    struct iovec *host_iov = alloca(iovcnt * sizeof(struct iovec));
    int64_t err = build_host_iov(g, iov_gva, iovcnt, host_iov);
    if (err < 0) return err;

    ssize_t ret = preadv(host_fd, host_iov, iovcnt, offset);
    return ret < 0 ? linux_errno() : ret;
}

int64_t sys_pwritev(guest_t *g, int fd, uint64_t iov_gva,
                    int iovcnt, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -LINUX_EINVAL;

    struct iovec *host_iov = alloca(iovcnt * sizeof(struct iovec));
    int64_t err = build_host_iov(g, iov_gva, iovcnt, host_iov);
    if (err < 0) return err;

    ssize_t ret = pwritev(host_fd, host_iov, iovcnt, offset);
    if (ret < 0) {
        if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        return linux_errno();
    }
    return ret;
}

int64_t sys_preadv2(guest_t *g, int fd, uint64_t iov_gva,
                    int iovcnt, int64_t offset, int flags) {
    /* preadv2 extends preadv with a flags parameter (RWF_HIPRI,
     * RWF_DSYNC, etc.). macOS has no preadv2 equivalent, so we
     * ignore the flags and delegate to preadv. If offset is -1,
     * use the current file position (like readv). */
    (void)flags;
    if (offset == -1)
        return sys_readv(g, fd, iov_gva, iovcnt);
    return sys_preadv(g, fd, iov_gva, iovcnt, offset);
}

int64_t sys_pwritev2(guest_t *g, int fd, uint64_t iov_gva,
                     int iovcnt, int64_t offset, int flags) {
    (void)flags;
    if (offset == -1)
        return sys_writev(g, fd, iov_gva, iovcnt);
    return sys_pwritev(g, fd, iov_gva, iovcnt, offset);
}

/* ---------- rosettad socket tracking ---------- */

/* Rosetta connects to rosettad via a Unix socket for AOT translation.
 * Since macOS doesn't support Linux abstract sockets, we use a socketpair
 * to intercept this communication. These functions track which host fd
 * is the rosettad socketpair end so we can intercept connect() and
 * monitor the protocol. */
static int rosettad_handler_fd = -1;  /* Our end of the socketpair */
static int rosettad_client_fd = -1;   /* Rosetta's end (host fd) */

/* Receive a file descriptor via SCM_RIGHTS ancillary data.
 * Also reads the normal data payload into buf (up to buflen).
 * Returns bytes of normal data received, or -1 on error.
 * On success, *recv_fd is set to the received fd (-1 if none). */
static ssize_t recv_fd(int sock, void *buf, size_t buflen, int *recv_fd_out) {
    struct iovec iov = { .iov_base = buf, .iov_len = buflen };
    uint8_t cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = cmsg_buf, .msg_controllen = sizeof(cmsg_buf)
    };

    *recv_fd_out = -1;
    ssize_t n = recvmsg(sock, &msg, 0);
    if (n <= 0) return n;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS) {
        memcpy(recv_fd_out, CMSG_DATA(cmsg), sizeof(int));
    }
    return n;
}

/* Send a file descriptor via SCM_RIGHTS ancillary data.
 * Also sends normal data from buf (buflen bytes).
 * Returns bytes sent or -1 on error. */
static ssize_t send_fd(int sock, const void *buf, size_t buflen, int send_fd_val) {
    struct iovec iov = { .iov_base = (void *)buf, .iov_len = buflen };
    uint8_t cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = cmsg_buf, .msg_controllen = sizeof(cmsg_buf)
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &send_fd_val, sizeof(int));

    return sendmsg(sock, &msg, 0);
}

/* Handle the rosettad protocol on our end of the socketpair.
 * Protocol:
 *   '?' → respond 0x01 (ready)
 *   't' → receive binary fd via SCM_RIGHTS, translate, send back AOT fd
 *   'd' → receive 32-byte digest, check cache, respond
 *   'q' → quit
 */
static void *rosettad_handler_thread(void *arg) {
    int fd = (int)(intptr_t)arg;

    for (;;) {
        uint8_t cmd;
        ssize_t n = read(fd, &cmd, 1);
        if (n <= 0) break;

        switch (cmd) {
        case '?': {
            /* Handshake: respond 0x01 (ready) */
            uint8_t resp = 0x01;
            write(fd, &resp, 1);
            break;
        }

        case 't': {
            /* Translate request: rosetta sends the binary fd via sendmsg
             * (SCM_RIGHTS) with a 1-byte data payload. */
            uint8_t params[256];
            int bin_fd = -1;

            recv_fd(fd, params, sizeof(params), &bin_fd);

            if (bin_fd < 0) {
                uint8_t resp = 0x00;
                write(fd, &resp, 1);
                break;
            }

            /* Get the binary's path via F_GETPATH */
            char bin_path[1024];
            if (fcntl(bin_fd, F_GETPATH, bin_path) < 0) {
                fprintf(stderr, "hl: rosettad: F_GETPATH failed: %s\n",
                        strerror(errno));
                uint8_t resp = 0x00;
                write(fd, &resp, 1);
                close(bin_fd);
                break;
            }
            close(bin_fd);

            /* Create temp file for AOT output */
            char aot_path[] = "/tmp/hl-aot-XXXXXX";
            int aot_fd = mkstemp(aot_path);
            if (aot_fd < 0) {
                uint8_t resp = 0x00;
                write(fd, &resp, 1);
                break;
            }
            close(aot_fd);

            /* Run rosettad translate via hl (it's a Linux binary).
             * proc_get_hl_path() returns the absolute path of the running
             * hl binary, resolved at startup via _NSGetExecutablePath(). */
            const char *hl_bin = proc_get_hl_path();
            if (!hl_bin) hl_bin = "hl";  /* fallback to PATH lookup */
            char cmd_buf[2048];
            snprintf(cmd_buf, sizeof(cmd_buf),
                     "'%s' /Library/Apple/usr/libexec/oah/RosettaLinux/rosettad "
                     "translate '%s' '%s' 2>/dev/null",
                     hl_bin, bin_path, aot_path);
            int ret = system(cmd_buf);

            if (ret != 0) {
                fprintf(stderr, "hl: rosettad: translate failed (%d) for %s\n",
                        ret, bin_path);
                uint8_t resp = 0x00;
                write(fd, &resp, 1);
                unlink(aot_path);
                break;
            }

            /* TODO: Compute real digest via rosettad digest command.
             * For now, send zeros — rosetta accepts this. */
            uint8_t digest[32] = {0};

            /* Open the AOT file and send it back */
            aot_fd = open(aot_path, O_RDONLY);
            if (aot_fd < 0) {
                uint8_t resp = 0x00;
                write(fd, &resp, 1);
                unlink(aot_path);
                break;
            }
            unlink(aot_path);  /* Remove temp file, fd keeps it alive */

            /* Send success + digest */
            uint8_t resp = 0x01;
            write(fd, &resp, 1);
            write(fd, digest, 32);

            /* Send AOT fd via SCM_RIGHTS.
             * The metadata payload format is unknown — send minimal data. */
            uint8_t meta[8] = {0};
            send_fd(fd, meta, sizeof(meta), aot_fd);
            close(aot_fd);
            break;
        }

        case 'd': {
            /* Digest lookup: receive 32-byte SHA256 */
            uint8_t digest[32];
            read(fd, digest, 32);
            /* We don't have a cache, respond 0x00 (not found) */
            uint8_t resp = 0x00;
            write(fd, &resp, 1);
            break;
        }

        case 'q':
            goto done;

        default:
            fprintf(stderr, "hl: rosettad: unknown cmd 0x%02x\n", cmd);
            goto done;
        }
    }

done:
    close(fd);
    return NULL;
}

void rosettad_set_socket(int fd) {
    rosettad_handler_fd = fd;
    /* Start background thread to monitor what rosetta sends */
    pthread_t thr;
    pthread_create(&thr, NULL, rosettad_handler_thread, (void *)(intptr_t)fd);
    pthread_detach(thr);
}

void rosettad_set_client_fd(int fd) {
    rosettad_client_fd = fd;
}

int rosettad_is_socket(int host_fd) {
    return rosettad_client_fd >= 0 && host_fd == rosettad_client_fd;
}

/* ---------- terminal I/O ---------- */

/* Rosetta Virtualization.framework ioctls — type 'a' (0x61).
 * In VZ Linux VMs, rosetta communicates with the macOS hypervisor via
 * custom ioctls on the virtiofs-mounted rosetta binary (opened via
 * /proc/self/exe). We emulate these to make rosetta work in HVF.
 *
 * 0x80456125: _IOR('a', 0x25, 69)  — environment signature check
 * 0x80806123: _IOR('a', 0x23, 128) — capabilities/config query
 * 0x6124:     _IO('a', 0x24)       — JIT activation / hypervisor handshake */
#define ROSETTA_VZ_CHECK      0x80456125
#define ROSETTA_VZ_CAPS       0x80806123
#define ROSETTA_VZ_ACTIVATE   0x6124

int64_t sys_ioctl(guest_t *g, int fd, uint64_t request, uint64_t arg) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    switch (request) {
    case ROSETTA_VZ_CHECK: {
        /* Rosetta environment check. Returns a 69-byte signature that
         * rosetta memcmp's against an embedded constant to verify it's
         * running in a supported Apple Virtualization.framework environment.
         * This MUST succeed — without it, rosetta prints "Rosetta is only
         * intended to run on Apple Silicon with a macOS host using
         * Virtualization.framework with Rosetta mode enabled" and aborts. */
        static const char rosetta_sig[69] =
            "Our hard work\nby these words guarded\n"
            "please don't steal\n\xc2\xa9 Apple Inc";
        if (guest_write(g, arg, rosetta_sig, sizeof(rosetta_sig)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    case ROSETTA_VZ_CAPS: {
        /* Rosetta capabilities query. Returns 128 bytes of host capability
         * data that controls rosetta's runtime behavior:
         *   caps[0]:   Master VZ enable flag (non-zero = VZ features on)
         *   caps[1]:   Sub-capability (read if caps[0]!=0 && caps[108]==0)
         *   caps[108]: Additional capability flag
         *   caps[109]: Stored separately as extra flag
         *   caps[3..108]: 106 bytes copied into rosetta internal state
         *
         * In VZ mode, rosetta enables AOT translation paths (loading
         * pre-translated .flu cache files) and may use rosettad for
         * ahead-of-time translation. The JIT code path remains the same
         * (ExecutableHeap at 0xf00000000000) regardless of VZ state.
         *
         * We return success with caps[0]=1 to enable VZ mode. */
        uint8_t caps[128] = {0};
        caps[0] = 1;   /* Enable VZ mode */
        caps[108] = 1;  /* Additional capability flag */
        if (guest_write(g, arg, caps, sizeof(caps)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    case ROSETTA_VZ_ACTIVATE:
        /* Rosetta JIT activation / hypervisor handshake. Returning 0
         * tells rosetta the VZ channel is active. On error, rosetta
         * handles EOPNOTSUPP (95) and ENOTTY (25) gracefully. */
        return 0;

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
        if (tcgetattr(host_fd, &t) < 0)
            return -LINUX_ENOTTY;  /* Not a terminal */
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
        int32_t val = (int32_t)pgrp;
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
        if (nw < 0) {
            if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
            if (total > 0) break;  /* Report partial success below */
            return linux_errno();
        }

        total += nw;
        remaining -= nw;
        if (offset >= 0) offset += nw;
        if (nw < nr) break;  /* Short write */
    }

    /* Write back updated offset (even on partial transfer) */
    if (offset_gva != 0) {
        if (guest_write(g, offset_gva, &offset, 8) < 0)
            return -LINUX_EFAULT;
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
        if (nw < 0) {
            if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
            if (total > 0) break;  /* Report partial success below */
            return linux_errno();
        }

        total += nw;
        remaining -= nw;
        if (off_in >= 0) off_in += nw;
        if (off_out >= 0) off_out += nw;
        if (nw < nr) break;
    }

    /* Write back updated offsets (even on partial transfer) */
    if (off_in_gva != 0) {
        if (guest_write(g, off_in_gva, &off_in, 8) < 0)
            return -LINUX_EFAULT;
    }
    if (off_out_gva != 0) {
        if (guest_write(g, off_out_gva, &off_out, 8) < 0)
            return -LINUX_EFAULT;
    }

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
            if (w <= 0) {
                if (w < 0 && errno == EPIPE) signal_queue(LINUX_SIGPIPE);
                goto done;
            }
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
        if (w < 0) {
            if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
            return total > 0 ? (int64_t)total : linux_errno();
        }
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
