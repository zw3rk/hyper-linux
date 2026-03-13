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
#include "rosetta.h"
#include "guest.h"
#include "thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <pthread.h>
#include <termios.h>
#include <CommonCrypto/CommonDigest.h>

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

/* ---------- termios flag translation helpers ---------- */

/* Linux aarch64 c_iflag bits (from asm-generic/termbits-common.h).
 * Low 9 bits (IGNBRK..ICRNL) match macOS exactly.
 * Bits from 0x200 onward differ: Linux IUCLC=0x200 has no macOS equivalent;
 * Linux IXON=0x400/IXOFF=0x1000 vs macOS IXON=0x200/IXOFF=0x400. */
#define LINUX_IFLAG_LOW_MASK  0x1ff   /* bits 0-8: same on Linux and macOS */
#define LINUX_IUCLC   0x0200         /* Linux only, no macOS equivalent */
#define LINUX_IXON    0x0400
#define LINUX_IXOFF   0x1000
#define LINUX_IMAXBEL 0x2000         /* same value on both */
#define LINUX_IUTF8   0x4000         /* same value on both */

/* Translate Linux c_iflag to macOS c_iflag. */
static tcflag_t linux_iflag_to_mac(uint32_t lf) {
    tcflag_t mf = lf & LINUX_IFLAG_LOW_MASK;   /* IGNBRK..ICRNL identical */
    /* IXANY=0x800 is the same on both; pass through */
    if (lf & 0x800)    mf |= IXANY;
    if (lf & LINUX_IXON)  mf |= IXON;          /* Linux 0x400 → macOS 0x200 */
    if (lf & LINUX_IXOFF) mf |= IXOFF;         /* Linux 0x1000 → macOS 0x400 */
    if (lf & LINUX_IMAXBEL) mf |= IMAXBEL;
    if (lf & LINUX_IUTF8)  mf |= IUTF8;
    /* IUCLC (Linux 0x200) has no macOS equivalent — drop it */
    return mf;
}

/* Translate macOS c_iflag to Linux c_iflag. */
static uint32_t mac_iflag_to_linux(tcflag_t mf) {
    uint32_t lf = mf & LINUX_IFLAG_LOW_MASK;   /* IGNBRK..ICRNL identical */
    if (mf & IXANY)   lf |= 0x800;
    if (mf & IXON)    lf |= LINUX_IXON;
    if (mf & IXOFF)   lf |= LINUX_IXOFF;
    if (mf & IMAXBEL) lf |= LINUX_IMAXBEL;
    if (mf & IUTF8)   lf |= LINUX_IUTF8;
    return lf;
}

/* Linux aarch64 c_oflag bits (asm-generic/termbits-common.h + termbits.h).
 * Only OPOST (0x01) has the same value on both platforms.
 * macOS 0x02 = ONLCR; Linux 0x02 = OLCUC (output lowercase→uppercase, rare).
 * macOS 0x04 = OXTABS; Linux 0x04 = ONLCR. All other bits shift by one. */
#define LINUX_OPOST   0x001
#define LINUX_OLCUC   0x002  /* Linux only (map lowercase→uppercase on output); macOS 0x02=ONLCR */
#define LINUX_ONLCR   0x004  /* macOS ONLCR=0x002 */
#define LINUX_OCRNL   0x008  /* macOS OCRNL=0x010 */
#define LINUX_ONOCR   0x010  /* macOS ONOCR=0x020 */
#define LINUX_ONLRET  0x020  /* macOS ONLRET=0x040 */
#define LINUX_OFILL   0x040  /* macOS OFILL=0x080 */
#define LINUX_OFDEL   0x080  /* macOS OFDEL=0x020000 */
/* Linux NLDLY/CRDLY/TABDLY/BSDLY/VTDLY/FFDLY have no macOS equivalents */

/* Translate Linux c_oflag to macOS c_oflag. */
static tcflag_t linux_oflag_to_mac(uint32_t lf) {
    tcflag_t mf = 0;
    if (lf & LINUX_OPOST)  mf |= OPOST;
    /* LINUX_OLCUC (0x002) has no macOS equivalent — drop it */
    if (lf & LINUX_ONLCR)  mf |= ONLCR;
    if (lf & LINUX_OCRNL)  mf |= OCRNL;
    if (lf & LINUX_ONOCR)  mf |= ONOCR;
    if (lf & LINUX_ONLRET) mf |= ONLRET;
    if (lf & LINUX_OFILL)  mf |= OFILL;
    if (lf & LINUX_OFDEL)  mf |= OFDEL;
    /* NLDLY, CRDLY, TABDLY, BSDLY, VTDLY, FFDLY: no macOS equivalents */
    return mf;
}

/* Translate macOS c_oflag to Linux c_oflag. */
static uint32_t mac_oflag_to_linux(tcflag_t mf) {
    uint32_t lf = 0;
    if (mf & OPOST)  lf |= LINUX_OPOST;
    if (mf & ONLCR)  lf |= LINUX_ONLCR;
    if (mf & OCRNL)  lf |= LINUX_OCRNL;
    if (mf & ONOCR)  lf |= LINUX_ONOCR;
    if (mf & ONLRET) lf |= LINUX_ONLRET;
    if (mf & OFILL)  lf |= LINUX_OFILL;
    if (mf & OFDEL)  lf |= LINUX_OFDEL;
    return lf;
}

/* Linux aarch64 c_cflag bits (asm-generic/termbits.h).
 * All standard flags differ from macOS — macOS shifts everything left by 4
 * bits (e.g., Linux CS8=0x30, macOS CS8=0x300; Linux CSTOPB=0x40, macOS=0x400).
 * The CBAUD field (Linux 0x0000100f) encodes baud rate symbolically; macOS
 * uses raw numeric speeds via cfgetispeed/cfsetispeed, so we drop CBAUD from
 * c_cflag and always use the speed accessors. */
#define LINUX_CSIZE   0x0030
#define LINUX_CS5     0x0000
#define LINUX_CS6     0x0010
#define LINUX_CS7     0x0020
#define LINUX_CS8     0x0030
#define LINUX_CSTOPB  0x0040
#define LINUX_CREAD   0x0080
#define LINUX_PARENB  0x0100
#define LINUX_PARODD  0x0200
#define LINUX_HUPCL   0x0400
#define LINUX_CLOCAL  0x0800
/* LINUX_CBAUD 0x0000100f and LINUX_CBAUDEX 0x00001000 encode baud in c_cflag;
 * macOS uses dedicated speed fields, so we ignore CBAUD on translation. */

/* Translate Linux c_cflag to macOS c_cflag. */
static tcflag_t linux_cflag_to_mac(uint32_t lf) {
    tcflag_t mf = 0;
    /* CSIZE: Linux CS5=0x00, CS6=0x10, CS7=0x20, CS8=0x30
     *        macOS CS5=0x00, CS6=0x100, CS7=0x200, CS8=0x300 */
    switch (lf & LINUX_CSIZE) {
    case LINUX_CS5: mf |= CS5; break;
    case LINUX_CS6: mf |= CS6; break;
    case LINUX_CS7: mf |= CS7; break;
    case LINUX_CS8: mf |= CS8; break;
    default: break;
    }
    if (lf & LINUX_CSTOPB) mf |= CSTOPB;
    if (lf & LINUX_CREAD)  mf |= CREAD;
    if (lf & LINUX_PARENB) mf |= PARENB;
    if (lf & LINUX_PARODD) mf |= PARODD;
    if (lf & LINUX_HUPCL)  mf |= HUPCL;
    if (lf & LINUX_CLOCAL) mf |= CLOCAL;
    /* CBAUD/CBAUDEX: drop — baud rate comes from c_ispeed/c_ospeed fields */
    return mf;
}

/* Translate macOS c_cflag to Linux c_cflag. */
static uint32_t mac_cflag_to_linux(tcflag_t mf) {
    uint32_t lf = 0;
    switch (mf & CSIZE) {
    case CS5: lf |= LINUX_CS5; break;
    case CS6: lf |= LINUX_CS6; break;
    case CS7: lf |= LINUX_CS7; break;
    case CS8: lf |= LINUX_CS8; break;
    default: break;
    }
    if (mf & CSTOPB) lf |= LINUX_CSTOPB;
    if (mf & CREAD)  lf |= LINUX_CREAD;
    if (mf & PARENB) lf |= LINUX_PARENB;
    if (mf & PARODD) lf |= LINUX_PARODD;
    if (mf & HUPCL)  lf |= LINUX_HUPCL;
    if (mf & CLOCAL) lf |= LINUX_CLOCAL;
    return lf;
}

/* Linux aarch64 c_lflag bits (asm-generic/termbits.h).
 * Virtually every flag has a different value from macOS.
 * Only ECHO (0x0008) is the same on both platforms. */
#define LINUX_ISIG    0x00001
#define LINUX_ICANON  0x00002
#define LINUX_XCASE   0x00004  /* Linux-only (rarely used) */
#define LINUX_ECHO    0x00008  /* same on macOS */
#define LINUX_ECHOE   0x00010
#define LINUX_ECHOK   0x00020
#define LINUX_ECHONL  0x00040
#define LINUX_NOFLSH  0x00080
#define LINUX_TOSTOP  0x00100
#define LINUX_ECHOCTL 0x00200
#define LINUX_ECHOPRT 0x00400
#define LINUX_ECHOKE  0x00800
#define LINUX_FLUSHO  0x01000
#define LINUX_PENDIN  0x04000
#define LINUX_IEXTEN  0x08000
#define LINUX_EXTPROC 0x10000

/* Translate Linux c_lflag to macOS c_lflag. */
static tcflag_t linux_lflag_to_mac(uint32_t lf) {
    tcflag_t mf = 0;
    if (lf & LINUX_ISIG)    mf |= ISIG;
    if (lf & LINUX_ICANON)  mf |= ICANON;
    /* LINUX_XCASE (0x004) has no macOS equivalent — drop it */
    if (lf & LINUX_ECHO)    mf |= ECHO;
    if (lf & LINUX_ECHOE)   mf |= ECHOE;
    if (lf & LINUX_ECHOK)   mf |= ECHOK;
    if (lf & LINUX_ECHONL)  mf |= ECHONL;
    if (lf & LINUX_NOFLSH)  mf |= NOFLSH;
    if (lf & LINUX_TOSTOP)  mf |= TOSTOP;
    if (lf & LINUX_ECHOCTL) mf |= ECHOCTL;
    if (lf & LINUX_ECHOPRT) mf |= ECHOPRT;
    if (lf & LINUX_ECHOKE)  mf |= ECHOKE;
    if (lf & LINUX_FLUSHO)  mf |= FLUSHO;
    if (lf & LINUX_PENDIN)  mf |= PENDIN;
    if (lf & LINUX_IEXTEN)  mf |= IEXTEN;
    if (lf & LINUX_EXTPROC) mf |= EXTPROC;
    return mf;
}

/* Translate macOS c_lflag to Linux c_lflag. */
static uint32_t mac_lflag_to_linux(tcflag_t mf) {
    uint32_t lf = 0;
    if (mf & ISIG)    lf |= LINUX_ISIG;
    if (mf & ICANON)  lf |= LINUX_ICANON;
    if (mf & ECHO)    lf |= LINUX_ECHO;
    if (mf & ECHOE)   lf |= LINUX_ECHOE;
    if (mf & ECHOK)   lf |= LINUX_ECHOK;
    if (mf & ECHONL)  lf |= LINUX_ECHONL;
    if (mf & NOFLSH)  lf |= LINUX_NOFLSH;
    if (mf & TOSTOP)  lf |= LINUX_TOSTOP;
    if (mf & ECHOCTL) lf |= LINUX_ECHOCTL;
    if (mf & ECHOPRT) lf |= LINUX_ECHOPRT;
    if (mf & ECHOKE)  lf |= LINUX_ECHOKE;
    if (mf & FLUSHO)  lf |= LINUX_FLUSHO;
    if (mf & PENDIN)  lf |= LINUX_PENDIN;
    if (mf & IEXTEN)  lf |= LINUX_IEXTEN;
    if (mf & EXTPROC) lf |= LINUX_EXTPROC;
    return lf;
}

/* ---------- basic read/write ---------- */

int64_t sys_write(guest_t *g, int fd, uint64_t buf_gva, uint64_t count) {
    if (fd >= 0 && fd < FD_TABLE_SIZE && fd_table[fd].type == FD_EVENTFD)
        return eventfd_write(fd, g, buf_gva, count);

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Linux: write(fd, NULL, 0) returns 0, not EFAULT */
    if (count == 0) return 0;

    /* Resolve buffer and cap count to available contiguous guest bytes.
     * guest_ptr_avail returns the host pointer and remaining bytes in
     * the current region — prevents host write() from reading past
     * the guest buffer boundary. */
    uint64_t avail = 0;
    void *buf = guest_ptr_avail(g, buf_gva, &avail);
    if (!buf) return -LINUX_EFAULT;
    if (count > avail) count = avail;

    ssize_t ret = write(host_fd, buf, count);
    if (ret < 0) {
        int saved_errno = errno;
        if (saved_errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        errno = saved_errno;
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

    /* Linux: read(fd, NULL, 0) returns 0, not EFAULT */
    if (count == 0) return 0;

    /* Resolve buffer and cap count to available contiguous guest bytes.
     * Prevents host read() from writing past the guest buffer boundary. */
    uint64_t avail = 0;
    void *buf = guest_ptr_avail(g, buf_gva, &avail);
    if (!buf) return -LINUX_EFAULT;
    if (count > avail) count = avail;

    ssize_t ret = read(host_fd, buf, count);
    return ret < 0 ? linux_errno() : ret;
}

int64_t sys_pread64(guest_t *g, int fd, uint64_t buf_gva,
                    uint64_t count, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Linux: pread(fd, NULL, 0, off) returns 0, not EFAULT */
    if (count == 0) return 0;

    uint64_t avail = 0;
    void *buf = guest_ptr_avail(g, buf_gva, &avail);
    if (!buf) return -LINUX_EFAULT;
    if (count > avail) count = avail;

    ssize_t ret = pread(host_fd, buf, count, offset);
    return ret < 0 ? linux_errno() : ret;
}

int64_t sys_pwrite64(guest_t *g, int fd, uint64_t buf_gva,
                     uint64_t count, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Linux: pwrite(fd, NULL, 0, off) returns 0, not EFAULT */
    if (count == 0) return 0;

    uint64_t avail = 0;
    void *buf = guest_ptr_avail(g, buf_gva, &avail);
    if (!buf) return -LINUX_EFAULT;
    if (count > avail) count = avail;

    ssize_t ret = pwrite(host_fd, buf, count, offset);
    if (ret < 0) {
        int saved_errno = errno;
        if (saved_errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        errno = saved_errno;
        return linux_errno();
    }
    return ret;
}

/* Helper: build host iovec array from guest iovec array.
 * Uses guest_read for the iovec array (may cross 2MB block boundary)
 * and guest_ptr_avail for each buffer (caps to contiguous bytes).
 * Returns 0 on success, -LINUX_EFAULT on bad guest pointer. */
static int64_t build_host_iov(guest_t *g, uint64_t iov_gva, int iovcnt,
                               struct iovec *host_iov) {
    linux_iovec_t guest_iov[1024]; /* UIO_MAXIOV */
    if (iovcnt > 1024) iovcnt = 1024;
    if (guest_read(g, iov_gva, guest_iov,
                   iovcnt * sizeof(linux_iovec_t)) < 0)
        return -LINUX_EFAULT;
    for (int i = 0; i < iovcnt; i++) {
        if (guest_iov[i].iov_len == 0) {
            host_iov[i].iov_base = NULL;
            host_iov[i].iov_len = 0;
            continue;
        }
        uint64_t avail = 0;
        void *base = guest_ptr_avail(g, guest_iov[i].iov_base, &avail);
        if (!base) return -LINUX_EFAULT;
        /* Cap to contiguous bytes within the 2MB block */
        uint64_t len = guest_iov[i].iov_len;
        if (len > avail) len = avail;
        host_iov[i].iov_base = base;
        host_iov[i].iov_len = len;
    }
    return 0;
}

int64_t sys_readv(guest_t *g, int fd, uint64_t iov_gva, int iovcnt) {
    /* Special FD types need their custom read handlers — glibc may use
     * readv() instead of read() for the same logical operation. Delegate
     * to the first iov entry's buffer.  Use the first iov's length (not
     * the sum of all iovs) because the data goes into giov[0].iov_base
     * which is only giov[0].iov_len bytes long. */
    if (fd >= 0 && fd < FD_TABLE_SIZE) {
        int type = fd_table[fd].type;
        if (type == FD_EVENTFD || type == FD_SIGNALFD ||
            type == FD_TIMERFD || type == FD_INOTIFY) {
            if (iovcnt <= 0) return -LINUX_EINVAL;
            /* Use guest_read for the iov array — guest_ptr alone is unsafe
             * if the array spans a 2MB block boundary. */
            linux_iovec_t giov;
            if (guest_read(g, iov_gva, &giov, sizeof(giov)) < 0)
                return -LINUX_EFAULT;
            return sys_read(g, fd, giov.iov_base, giov.iov_len);
        }
    }

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -LINUX_EINVAL;

    struct iovec *host_iov = alloca(iovcnt * sizeof(struct iovec));
    int64_t err = build_host_iov(g, iov_gva, iovcnt, host_iov);
    if (err < 0) return err;

    ssize_t ret = readv(host_fd, host_iov, iovcnt);
    return ret < 0 ? linux_errno() : ret;
}

int64_t sys_writev(guest_t *g, int fd, uint64_t iov_gva, int iovcnt) {
    /* Special FD types: glibc may use writev() for eventfd wakeup writes.
     * Delegate using the first iov entry.  Use giov.iov_len (not the
     * sum of all iovs) — the data is at giov.iov_base which is only
     * giov.iov_len bytes.  eventfd expects exactly 8 bytes. */
    if (fd >= 0 && fd < FD_TABLE_SIZE && fd_table[fd].type == FD_EVENTFD) {
        if (iovcnt <= 0) return -LINUX_EINVAL;
        linux_iovec_t giov;
        if (guest_read(g, iov_gva, &giov, sizeof(giov)) < 0)
            return -LINUX_EFAULT;
        return eventfd_write(fd, g, giov.iov_base, giov.iov_len);
    }

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (iovcnt <= 0 || iovcnt > 1024) return -LINUX_EINVAL;

    struct iovec *host_iov = alloca(iovcnt * sizeof(struct iovec));
    int64_t err = build_host_iov(g, iov_gva, iovcnt, host_iov);
    if (err < 0) return err;

    ssize_t ret = writev(host_fd, host_iov, iovcnt);
    if (ret < 0) {
        int saved_errno = errno;
        if (saved_errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        errno = saved_errno;
        return linux_errno();
    }
    return ret;
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
        int saved_errno = errno;
        if (saved_errno == EPIPE) signal_queue(LINUX_SIGPIPE);
        errno = saved_errno;
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

/* Rosetta connects to rosettad via AF_UNIX SOCK_SEQPACKET for AOT
 * (ahead-of-time) translation. macOS doesn't support SOCK_SEQPACKET
 * for AF_UNIX, so we intercept the socket creation in sys_socket()
 * (syscall_net.c) with a socketpair(SOCK_STREAM). One end goes to
 * rosetta (the client), the other to our rosettad_handler_thread.
 * connect() is intercepted to return success immediately. */
static char rosettad_binary_path[LINUX_PATH_MAX] = {0}; /* x86_64 binary for on-demand AOT */
static int rosettad_client_fd = -1;  /* Rosetta's end of socketpair (host fd) */

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
        cmsg->cmsg_type == SCM_RIGHTS &&
        cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
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
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
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

/* Compute SHA256 digest of a file by fd (seeks back to start after).
 * Returns 0 on success, -1 on error. */
static int compute_fd_sha256(int fd, uint8_t digest[CC_SHA256_DIGEST_LENGTH]) {
    off_t saved = lseek(fd, 0, SEEK_CUR);
    lseek(fd, 0, SEEK_SET);

    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);

    uint8_t buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        CC_SHA256_Update(&ctx, buf, (CC_LONG)n);

    lseek(fd, saved, SEEK_SET);
    if (n < 0) return -1;

    CC_SHA256_Final(digest, &ctx);
    return 0;
}

/* ---------- Persistent AOT cache ---------- */

/* Cache directory: ~/.cache/hl-rosettad/
 * Files: <sha256_hex>.aot — keyed by SHA256 of the original x86_64 binary.
 * This matches real rosettad behavior: the digest rosetta stores in .flu
 * files (and sends via 'd') is the SHA256 of the binary, not the AOT. */
static char aot_cache_dir[PATH_MAX] = {0};

/* Initialize the persistent AOT cache directory.
 * Creates ~/.cache/hl-rosettad/ if it doesn't exist. */
static void aot_cache_init(void) {
    if (aot_cache_dir[0]) return;  /* already initialized */

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    snprintf(aot_cache_dir, sizeof(aot_cache_dir),
             "%s/" ROSETTAD_CACHE_SUBDIR, home);
    mkdir(aot_cache_dir, 0700);  /* ignore EEXIST */
}

/* Format a SHA256 digest as hex into buf (must be >= ROSETTAD_DIGEST_HEX_LEN). */
static void digest_to_hex(const uint8_t digest[ROSETTAD_DIGEST_SIZE], char *buf) {
    for (int i = 0; i < ROSETTAD_DIGEST_SIZE; i++)
        snprintf(buf + (size_t)i * 2, 3, "%02x", digest[i]);
}

/* Look up a cached AOT file by binary SHA256 digest.
 * Returns an open fd on hit, -1 on miss. */
static int aot_cache_lookup(const uint8_t digest[ROSETTAD_DIGEST_SIZE]) {
    aot_cache_init();
    char hex[ROSETTAD_DIGEST_HEX_LEN];
    digest_to_hex(digest, hex);

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.aot", aot_cache_dir, hex);
    return open(path, O_RDONLY);
}

/* Store an AOT file in the persistent cache, keyed by binary SHA256.
 * Moves (hard-links + unlinks) the temp file into the cache dir. */
static void aot_cache_store(const uint8_t digest[ROSETTAD_DIGEST_SIZE],
                            const char *aot_path) {
    aot_cache_init();
    char hex[ROSETTAD_DIGEST_HEX_LEN];
    digest_to_hex(digest, hex);

    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/%s.aot", aot_cache_dir, hex);

    /* Try link+unlink for atomicity; fall back to rename */
    if (link(aot_path, dest) == 0) {
        unlink(aot_path);
    } else {
        rename(aot_path, dest);  /* cross-device fallback */
    }
}

/* Run 'hl rosettad translate <input> <output>' via posix_spawn().
 * Returns the child exit status, or -1 on spawn failure. */
static int run_rosettad_translate(const char *bin_path, const char *aot_path) {
    const char *hl_bin = proc_get_hl_path();
    if (!hl_bin) hl_bin = "hl";

    static const char rosettad_path[] =
        "/Library/Apple/usr/libexec/oah/RosettaLinux/rosettad";

    char *argv[] = {
        (char *)hl_bin, (char *)rosettad_path,
        "translate", (char *)bin_path, (char *)aot_path, NULL
    };

    /* Let child inherit stderr for diagnostic output */
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    extern char **environ;
    pid_t pid;
    int err = posix_spawn(&pid, hl_bin, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    if (err != 0) {
        fprintf(stderr, "hl: rosettad: posix_spawn failed: %s\n",
                strerror(err));
        return -1;
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Handle the rosettad protocol on our end of the socketpair.
 *
 * Rosetta connects to rosettad via AF_UNIX SOCK_SEQPACKET for AOT
 * (ahead-of-time) translation of x86_64 code. Since macOS doesn't
 * support SOCK_SEQPACKET for AF_UNIX, we intercept via socketpair
 * (SOCK_STREAM) and frame messages by individual write() calls.
 *
 * Protocol (command constants in rosetta.h):
 *   ROSETTAD_CMD_HANDSHAKE → respond ROSETTAD_RESP_HIT (ready)
 *   ROSETTAD_CMD_TRANSLATE → receive binary fd via SCM_RIGHTS, translate, send AOT fd
 *   ROSETTAD_CMD_DIGEST    → receive ROSETTAD_DIGEST_SIZE-byte SHA256, check cache
 *   ROSETTAD_CMD_QUIT      → exit handler thread
 */
static void *rosettad_handler_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    if (hl_verbose)
        fprintf(stderr, "hl: rosettad: handler thread started (fd=%d)\n", fd);

    for (;;) {
        uint8_t cmd;
        ssize_t n = read(fd, &cmd, 1);
        if (n <= 0) {
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: read returned %zd (%s), "
                        "exiting\n", n, n < 0 ? strerror(errno) : "EOF");
            break;
        }

        switch (cmd) {
        case ROSETTAD_CMD_HANDSHAKE: {
            /* Handshake: respond ROSETTAD_RESP_HIT to enable AOT translation */
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: handshake '?'\n");
            uint8_t resp = ROSETTAD_RESP_HIT;
            if (write(fd, &resp, 1) != 1) {
                fprintf(stderr, "hl: rosettad: handshake write failed\n");
                goto done;
            }
            break;
        }

        case ROSETTAD_CMD_TRANSLATE: {
            /* Translate request: rosetta sends the binary fd via sendmsg
             * (SCM_RIGHTS) with a data payload. We compute the binary's
             * SHA256, check the persistent cache, and translate only on
             * cache miss. This matches real rosettad behavior where the
             * digest is always the SHA256 of the original binary. */
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: translate request 't'\n");
            uint8_t params[256];
            int bin_fd = -1;

            ssize_t rn = recv_fd(fd, params, sizeof(params), &bin_fd);
            if (rn <= 0 || bin_fd < 0) {
                fprintf(stderr, "hl: rosettad: recv_fd failed: n=%zd fd=%d (%s)\n",
                        rn, bin_fd, rn < 0 ? strerror(errno) : "no fd");
                if (bin_fd >= 0) close(bin_fd);
                uint8_t resp = ROSETTAD_RESP_MISS;
                if (write(fd, &resp, 1) != 1) goto done;
                break;
            }
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: recv_fd got fd=%d, %zd bytes data\n",
                        bin_fd, rn);

            /* Compute SHA256 of the original binary (not the AOT output).
             * This is the digest rosetta stores in .flu files and sends
             * via 'd' for subsequent cache lookups. */
            uint8_t bin_digest[ROSETTAD_DIGEST_SIZE];
            if (compute_fd_sha256(bin_fd, bin_digest) < 0) {
                fprintf(stderr, "hl: rosettad: SHA256 of binary failed\n");
                close(bin_fd);
                uint8_t resp = ROSETTAD_RESP_MISS;
                if (write(fd, &resp, 1) != 1) goto done;
                break;
            }

            /* Check persistent cache — skip translation if already cached */
            int cached_fd = aot_cache_lookup(bin_digest);
            if (cached_fd >= 0) {
                if (hl_verbose) {
                    char hex[ROSETTAD_DIGEST_HEX_LEN];
                    digest_to_hex(bin_digest, hex);
                    fprintf(stderr, "hl: rosettad: cache HIT for %s\n", hex);
                }
                close(bin_fd);

                /* Send cached AOT: success + digest + fd */
                uint8_t resp = ROSETTAD_RESP_HIT;
                if (write(fd, &resp, 1) != 1) { close(cached_fd); goto done; }
                if (write(fd, bin_digest, ROSETTAD_DIGEST_SIZE) != ROSETTAD_DIGEST_SIZE) {
                    close(cached_fd); goto done;
                }
                uint8_t dummy = 0;
                ssize_t sent = send_fd(fd, &dummy, 1, cached_fd);
                if (sent < 0) {
                    fprintf(stderr, "hl: rosettad: send_fd (cached) failed: %s\n",
                            strerror(errno));
                    close(cached_fd);
                    goto done;
                }
                if (hl_verbose)
                    fprintf(stderr, "hl: rosettad: sent cached AOT fd=%d\n",
                            cached_fd);
                close(cached_fd);
                break;
            }

            /* Get the binary's path via F_GETPATH for translation */
            char bin_path[1024];
            if (fcntl(bin_fd, F_GETPATH, bin_path) < 0) {
                fprintf(stderr, "hl: rosettad: F_GETPATH failed: %s\n",
                        strerror(errno));
                uint8_t resp = ROSETTAD_RESP_MISS;
                if (write(fd, &resp, 1) != 1) { close(bin_fd); goto done; }
                close(bin_fd);
                break;
            }
            close(bin_fd);
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: translating %s\n", bin_path);

            /* Create temp file for AOT output */
            char aot_path[] = "/tmp/hl-aot-XXXXXX";
            int aot_fd = mkstemp(aot_path);
            if (aot_fd < 0) {
                fprintf(stderr, "hl: rosettad: mkstemp failed: %s\n",
                        strerror(errno));
                uint8_t resp = ROSETTAD_RESP_MISS;
                if (write(fd, &resp, 1) != 1) goto done;
                break;
            }
            close(aot_fd);

            int ret = run_rosettad_translate(bin_path, aot_path);
            if (ret != 0) {
                fprintf(stderr, "hl: rosettad: translate failed (exit=%d) "
                        "for %s\n", ret, bin_path);
                uint8_t resp = ROSETTAD_RESP_MISS;
                if (write(fd, &resp, 1) != 1) { unlink(aot_path); goto done; }
                unlink(aot_path);
                break;
            }

            /* Store AOT in persistent cache (moves temp file into cache dir) */
            aot_cache_store(bin_digest, aot_path);

            /* Open the cached AOT file for sending */
            aot_fd = aot_cache_lookup(bin_digest);
            if (aot_fd < 0) {
                /* Fallback: try the original temp path (store may have failed) */
                aot_fd = open(aot_path, O_RDONLY);
            }
            if (aot_fd < 0) {
                fprintf(stderr, "hl: rosettad: open AOT failed after translate\n");
                uint8_t resp = ROSETTAD_RESP_MISS;
                if (write(fd, &resp, 1) != 1) goto done;
                break;
            }
            if (hl_verbose) {
                struct stat st;
                if (fstat(aot_fd, &st) == 0)
                    fprintf(stderr, "hl: rosettad: AOT ready (%lld bytes) for %s\n",
                            (long long)st.st_size, bin_path);
            }

            /* Rosetta expects THREE separate messages for the translate
             * response (matching SOCK_SEQPACKET semantics where each
             * send/write creates a distinct message):
             *   1. Success byte (ROSETTAD_RESP_HIT)
             *   2. SHA256 digest of original binary (ROSETTAD_DIGEST_SIZE bytes)
             *   3. AOT fd via SCM_RIGHTS + 1-byte dummy
             *
             * IMPORTANT: Do NOT combine into one sendmsg — rosetta reads
             * these as three separate recvmsg/read calls. */
            uint8_t resp = ROSETTAD_RESP_HIT;
            if (write(fd, &resp, 1) != 1) { close(aot_fd); goto done; }
            if (write(fd, bin_digest, ROSETTAD_DIGEST_SIZE) != ROSETTAD_DIGEST_SIZE) {
                close(aot_fd); goto done;
            }

            /* Send AOT fd via SCM_RIGHTS with 1-byte dummy payload. */
            uint8_t dummy = 0;
            ssize_t sent = send_fd(fd, &dummy, 1, aot_fd);
            if (sent < 0) {
                fprintf(stderr, "hl: rosettad: send_fd failed: %s\n",
                        strerror(errno));
                close(aot_fd);
                goto done;
            }
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: sent AOT fd=%d (%zd bytes meta)\n",
                        aot_fd, sent);
            close(aot_fd);
            break;
        }

        case ROSETTAD_CMD_DIGEST: {
            /* Digest lookup: receive 32-byte SHA256 of the original binary,
             * look up the persistent AOT cache.
             *
             * This matches real rosettad behavior: rosetta caches the binary
             * SHA256 in .flu files (~/.cache/rosetta/) and sends it via 'd'
             * on subsequent invocations. On HIT, we send the cached AOT fd
             * directly — this avoids re-translation and uses the 'd' HIT
             * code path in rosetta (which handles large binaries better
             * than the 't' response path). */
            uint8_t digest[ROSETTAD_DIGEST_SIZE];
            size_t dgst_off = 0;
            while (dgst_off < ROSETTAD_DIGEST_SIZE) {
                ssize_t nr = read(fd, digest + dgst_off,
                                  ROSETTAD_DIGEST_SIZE - dgst_off);
                if (nr < 0 && errno == EINTR) continue;
                if (nr <= 0) break;
                dgst_off += nr;
            }
            if (dgst_off != ROSETTAD_DIGEST_SIZE) {
                fprintf(stderr, "hl: rosettad: digest read short\n");
                goto done;
            }

            int cached_fd = aot_cache_lookup(digest);
            if (cached_fd >= 0) {
                if (hl_verbose) {
                    char hex[ROSETTAD_DIGEST_HEX_LEN];
                    digest_to_hex(digest, hex);
                    fprintf(stderr, "hl: rosettad: digest lookup → HIT (%s)\n",
                            hex);
                }

                /* HIT response: success byte + AOT fd via SCM_RIGHTS */
                uint8_t resp = ROSETTAD_RESP_HIT;
                if (write(fd, &resp, 1) != 1) { close(cached_fd); goto done; }
                uint8_t dummy = 0;
                ssize_t sent = send_fd(fd, &dummy, 1, cached_fd);
                if (sent < 0) {
                    fprintf(stderr, "hl: rosettad: send_fd (digest) failed: %s\n",
                            strerror(errno));
                    close(cached_fd);
                    goto done;
                }
                if (hl_verbose)
                    fprintf(stderr, "hl: rosettad: sent cached AOT fd=%d\n",
                            cached_fd);
                close(cached_fd);
            } else {
                if (hl_verbose) {
                    char hex[ROSETTAD_DIGEST_HEX_LEN];
                    digest_to_hex(digest, hex);
                    fprintf(stderr, "hl: rosettad: digest lookup → MISS (%s)\n",
                            hex);
                }
                uint8_t resp = ROSETTAD_RESP_MISS;
                if (write(fd, &resp, 1) != 1) goto done;
            }
            break;
        }

        case ROSETTAD_CMD_QUIT:
            if (hl_verbose)
                fprintf(stderr, "hl: rosettad: quit 'q'\n");
            goto done;

        default:
            fprintf(stderr, "hl: rosettad: unknown cmd 0x%02x\n", cmd);
            goto done;
        }
    }

done:
    if (hl_verbose)
        fprintf(stderr, "hl: rosettad: handler thread exiting\n");
    close(fd);
    rosettad_client_fd = -1;  /* Reset so rosettad_is_socket() stops matching */
    return NULL;
}

void rosettad_set_binary_path(const char *path) {
    if (path) {
        strncpy(rosettad_binary_path, path, sizeof(rosettad_binary_path) - 1);
        rosettad_binary_path[sizeof(rosettad_binary_path) - 1] = '\0';
    }
}

void rosettad_start_handler(int handler_fd, int client_fd) {
    rosettad_client_fd = client_fd;
    if (hl_verbose)
        fprintf(stderr, "hl: rosettad: starting handler thread "
                "(handler_fd=%d, client_fd=%d)\n", handler_fd, client_fd);
    pthread_t thr;
    pthread_create(&thr, NULL, rosettad_handler_thread,
                   (void *)(intptr_t)handler_fd);
    pthread_detach(thr);
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
/* VZ ioctl constants: centralized in rosetta.h */

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
        if (g->verbose)
            fprintf(stderr, "hl: rosetta: VZ_CHECK ioctl\n");
        static const char rosetta_sig[ROSETTA_VZ_SIG_LEN] =
            "Our hard work\nby these words guarded\n"
            "please don't steal\n\xc2\xa9 Apple Inc";
        if (guest_write(g, arg, rosetta_sig, sizeof(rosetta_sig)) < 0)
            return -LINUX_EFAULT;
        return 1;  /* Real VZ driver returns 1 on success */
    }

    case ROSETTA_VZ_CAPS: {
        /* VZ_CAPS buffer layout (ROSETTA_CAPS_SIZE bytes).
         * Offsets defined in rosetta.h.  Verified via strace in a real Lima VZ VM. */
        uint8_t caps[ROSETTA_CAPS_SIZE] = {0};

        /* caps[0]: VZ enable flag — activates the rosettad AOT pipeline. */
        caps[ROSETTA_CAPS_VZ_ENABLE] = 1;

        /* caps[1..]: Socket path — must be non-empty for rosettad init.
         * We use a short placeholder; connect() is intercepted fd-based. */
        static const char fake_sock_path[] = "/run/rosettad/rosetta.sock";
        memcpy(&caps[ROSETTA_CAPS_SOCKET_PATH], fake_sock_path,
               sizeof(fake_sock_path));

        /* caps[66..]: Null-terminated path to x86_64 binary for rosettad.
         * Rosetta opens this file and sends the fd to rosettad via SCM_RIGHTS. */
        size_t binpath_len = strlen(rosettad_binary_path);
        if (binpath_len > 0 &&
            binpath_len <= ROSETTA_CAPS_BINARY_PATH_LEN) {
            memcpy(&caps[ROSETTA_CAPS_BINARY_PATH], rosettad_binary_path,
                   binpath_len + 1);
        } else if (binpath_len > 0) {
            /* Path too long to fit: truncate, NUL-terminate within field */
            memcpy(&caps[ROSETTA_CAPS_BINARY_PATH], rosettad_binary_path,
                   ROSETTA_CAPS_BINARY_PATH_LEN);
            caps[ROSETTA_CAPS_BINARY_PATH + ROSETTA_CAPS_BINARY_PATH_LEN - 1] = 0;
        }

        /* caps[108]: Match real VZ behavior (0). Both 't' and 'd' protocol
         * commands work with this value, verified by strace in Lima VM. */
        caps[ROSETTA_CAPS_VZ_SECONDARY] = 0;

        if (g->verbose)
            fprintf(stderr, "hl: rosetta: VZ_CAPS ioctl → caps[0]=%d caps[64]=0x%02x "
                    "caps[108]=0x%02x binary=%s\n",
                    caps[ROSETTA_CAPS_VZ_ENABLE], caps[64],
                    caps[ROSETTA_CAPS_VZ_SECONDARY], rosettad_binary_path);
        if (guest_write(g, arg, caps, sizeof(caps)) < 0)
            return -LINUX_EFAULT;
        return 1;  /* Real VZ driver returns 1 on success */
    }

    case ROSETTA_VZ_ACTIVATE:
        /* Rosetta JIT activation / hypervisor handshake. */
        if (g->verbose)
            fprintf(stderr, "hl: rosetta: VZ_ACTIVATE ioctl\n");
        return 1;  /* Real VZ driver returns 1 on success */

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
        /* Get terminal attributes.
         * Linux and macOS use different c_cc index assignments for control
         * characters (e.g., Linux VINTR=0, macOS VINTR=8). Must translate. */
        struct termios t;
        if (tcgetattr(host_fd, &t) < 0)
            return -LINUX_ENOTTY;
        /* macOS c_cc index → Linux c_cc index mapping.
         * Linux: VINTR=0 VQUIT=1 VERASE=2 VKILL=3 VEOF=4 VTIME=5
         *        VMIN=6 VSWTC=7 VSTART=8 VSTOP=9 VSUSP=10 VEOL=11
         *        VREPRINT=12 VDISCARD=13 VWERASE=14 VLNEXT=15 VEOL2=16
         * macOS: VEOF=0 VEOL=1 VEOL2=2 VERASE=3 VWERASE=4 VKILL=5
         *        VREPRINT=6 (7=spare) VINTR=8 VQUIT=9 VSUSP=10 VDSUSP=11
         *        VSTART=12 VSTOP=13 VLNEXT=14 VDISCARD=15 VMIN=16 VTIME=17 */
        static const int mac_to_linux_cc[19] = {
            /*[linux 0  VINTR]    = mac*/  8,
            /*[linux 1  VQUIT]    = mac*/  9,
            /*[linux 2  VERASE]   = mac*/  3,
            /*[linux 3  VKILL]    = mac*/  5,
            /*[linux 4  VEOF]     = mac*/  0,
            /*[linux 5  VTIME]    = mac*/ 17,
            /*[linux 6  VMIN]     = mac*/ 16,
            /*[linux 7  VSWTC]    = mac*/ -1, /* no macOS equivalent */
            /*[linux 8  VSTART]   = mac*/ 12,
            /*[linux 9  VSTOP]    = mac*/ 13,
            /*[linux 10 VSUSP]    = mac*/ 10,
            /*[linux 11 VEOL]     = mac*/  1,
            /*[linux 12 VREPRINT] = mac*/  6,
            /*[linux 13 VDISCARD] = mac*/ 15,
            /*[linux 14 VWERASE]  = mac*/  4,
            /*[linux 15 VLNEXT]   = mac*/ 14,
            /*[linux 16 VEOL2]    = mac*/  2,
            -1, -1, /* unused slots 17-18 */
        };
        linux_termios_t lt = {0};
        lt.c_iflag = mac_iflag_to_linux(t.c_iflag);
        lt.c_oflag = mac_oflag_to_linux(t.c_oflag);
        lt.c_cflag = mac_cflag_to_linux(t.c_cflag);
        lt.c_lflag = mac_lflag_to_linux(t.c_lflag);
        for (int i = 0; i < 19; i++) {
            int mac_idx = mac_to_linux_cc[i];
            lt.c_cc[i] = (mac_idx >= 0 && mac_idx < NCCS) ? t.c_cc[mac_idx] : 0;
        }
        lt.c_ispeed = (uint32_t)cfgetispeed(&t);
        lt.c_ospeed = (uint32_t)cfgetospeed(&t);
        if (guest_write(g, arg, &lt, sizeof(lt)) < 0)
            return -LINUX_EFAULT;
        return 0;
    }

    case LINUX_TCSETS:
    case LINUX_TCSETSW:
    case LINUX_TCSETSF: {
        /* Set terminal attributes with c_cc index translation (see TCGETS) */
        static const int linux_to_mac_cc[19] = {
            /*[linux 0  VINTR]    → mac*/  8,
            /*[linux 1  VQUIT]    → mac*/  9,
            /*[linux 2  VERASE]   → mac*/  3,
            /*[linux 3  VKILL]    → mac*/  5,
            /*[linux 4  VEOF]     → mac*/  0,
            /*[linux 5  VTIME]    → mac*/ 17,
            /*[linux 6  VMIN]     → mac*/ 16,
            /*[linux 7  VSWTC]    → mac*/ -1,
            /*[linux 8  VSTART]   → mac*/ 12,
            /*[linux 9  VSTOP]    → mac*/ 13,
            /*[linux 10 VSUSP]    → mac*/ 10,
            /*[linux 11 VEOL]     → mac*/  1,
            /*[linux 12 VREPRINT] → mac*/  6,
            /*[linux 13 VDISCARD] → mac*/ 15,
            /*[linux 14 VWERASE]  → mac*/  4,
            /*[linux 15 VLNEXT]   → mac*/ 14,
            /*[linux 16 VEOL2]    → mac*/  2,
            -1, -1,
        };
        linux_termios_t lt;
        if (guest_read(g, arg, &lt, sizeof(lt)) < 0)
            return -LINUX_EFAULT;
        struct termios t;
        if (tcgetattr(host_fd, &t) < 0)
            return -LINUX_ENOTTY;  /* Not a terminal */
        t.c_iflag = linux_iflag_to_mac(lt.c_iflag);
        t.c_oflag = linux_oflag_to_mac(lt.c_oflag);
        t.c_cflag = linux_cflag_to_mac(lt.c_cflag);
        t.c_lflag = linux_lflag_to_mac(lt.c_lflag);
        for (int i = 0; i < 19; i++) {
            int mac_idx = linux_to_mac_cc[i];
            if (mac_idx >= 0 && mac_idx < NCCS)
                t.c_cc[mac_idx] = lt.c_cc[i];
        }
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

    /* Linux validates offset >= 0 and len > 0 */
    if (offset < 0 || len <= 0) return -LINUX_EINVAL;

    /* mode 0 = basic allocation → ftruncate fallback.
     * Other modes (FALLOC_FL_PUNCH_HOLE etc.) not supported. */
    if (mode != 0) return -LINUX_EOPNOTSUPP;

    struct stat st;
    if (fstat(host_fd, &st) < 0) return linux_errno();

    /* Extend file if needed (ftruncate only extends, doesn't shrink) */
    int64_t new_size = offset + len;
    if (new_size < offset) return -LINUX_EFBIG;  /* Overflow check */
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
        if (offset < 0) return -LINUX_EINVAL;
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
        if (nr < 0) {
            if (total > 0) break;  /* Partial success: report bytes sent */
            return linux_errno();
        }
        if (nr == 0) break;  /* EOF */

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

    /* Write back updated offset (even on partial transfer).
     * Preserve partial success: if bytes were transferred but offset
     * writeback fails, return the count rather than -EFAULT. */
    if (offset_gva != 0) {
        if (guest_write(g, offset_gva, &offset, 8) < 0)
            return total > 0 ? (int64_t)total : -LINUX_EFAULT;
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
        if (nr < 0) {
            if (total > 0) break;  /* Partial success: report bytes sent */
            return linux_errno();
        }
        if (nr == 0) break;  /* EOF */

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

    /* Write back updated offsets (even on partial transfer).
     * Preserve partial success on writeback failure. */
    if (off_in_gva != 0) {
        if (guest_write(g, off_in_gva, &off_in, 8) < 0)
            return total > 0 ? (int64_t)total : -LINUX_EFAULT;
    }
    if (off_out_gva != 0) {
        if (guest_write(g, off_out_gva, &off_out, 8) < 0)
            return total > 0 ? (int64_t)total : -LINUX_EFAULT;
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

    /* Emulate with read/write loop using a stack buffer (matching
     * sendfile/copy_file_range which also use stack buffers). */
    uint8_t buf[65536];
    size_t chunk = len > sizeof(buf) ? sizeof(buf) : len;

    size_t total = 0;
    int saved_errno = 0;  /* Preserve errno across guest_write */
    int rw_error = 0;     /* Track whether read or write failed */
    while (total < len) {
        size_t n = (len - total) > chunk ? chunk : (len - total);
        ssize_t r = (off_in >= 0) ? pread(host_in, buf, n, off_in)
                                  : read(host_in, buf, n);
        if (r < 0) { rw_error = 1; saved_errno = errno; break; }
        if (r == 0) break;  /* EOF */
        if (off_in >= 0) off_in += r;

        size_t written = 0;
        while (written < (size_t)r) {
            ssize_t w = (off_out >= 0)
                ? pwrite(host_out, buf + written, r - written, off_out)
                : write(host_out, buf + written, r - written);
            if (w <= 0) {
                if (w < 0) { rw_error = 1; saved_errno = errno; }
                if (w < 0 && saved_errno == EPIPE) signal_queue(LINUX_SIGPIPE);
                total += written;  /* Account for partial bytes written */
                goto done;
            }
            written += w;
            if (off_out >= 0) off_out += w;
        }
        total += r;
    }

done:
    /* Write back updated offsets. Preserve partial transfer success:
     * if bytes were already moved, return that count even if the
     * offset writeback fails (consistent with sendfile/copy_file_range). */
    if (off_in_gva && off_in >= 0 &&
        guest_write(g, off_in_gva, &off_in, 8) < 0)
        return total > 0 ? (int64_t)total : -LINUX_EFAULT;
    if (off_out_gva && off_out >= 0 &&
        guest_write(g, off_out_gva, &off_out, 8) < 0)
        return total > 0 ? (int64_t)total : -LINUX_EFAULT;

    /* Return bytes transferred, or errno only if read/write failed.
     * Restore saved_errno since free/guest_write may have clobbered it. */
    if (total > 0) return (int64_t)total;
    if (rw_error) { errno = saved_errno; return linux_errno(); }
    return 0;
}

/* vmsplice: emulate as writev to the pipe fd */
int64_t sys_vmsplice(guest_t *g, int fd, uint64_t iov_gva,
                     unsigned long nr_segs, unsigned int flags) {
    (void)flags;
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (nr_segs > 1024) return -LINUX_EINVAL;  /* UIO_MAXIOV */
    if (nr_segs > 64) nr_segs = 64;  /* local processing limit */

    size_t total = 0;
    for (unsigned long i = 0; i < nr_segs; i++) {
        linux_iovec_t liov;
        if (guest_read(g, iov_gva + i * sizeof(linux_iovec_t),
                       &liov, sizeof(liov)) < 0)
            return -LINUX_EFAULT;

        if (liov.iov_len == 0) continue;
        uint64_t avail = 0;
        void *src = guest_ptr_avail(g, liov.iov_base, &avail);
        if (!src) return total > 0 ? (int64_t)total : -LINUX_EFAULT;
        uint64_t len = liov.iov_len;
        if (len > avail) len = avail;

        ssize_t w = write(host_fd, src, len);
        if (w < 0) {
            if (errno == EPIPE) signal_queue(LINUX_SIGPIPE);
            return total > 0 ? (int64_t)total : linux_errno();
        }
        total += w;
        if ((uint64_t)w < len) break;
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
