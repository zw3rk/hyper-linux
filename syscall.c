/* syscall.c — Linux aarch64 syscall dispatch and handlers for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Translates Linux aarch64 syscalls into macOS equivalents. The guest
 * binary issues SVC #0, the shim forwards via HVC #5, and this code
 * reads vCPU registers, executes the syscall on the host, and writes
 * the result back.
 */
#include "syscall.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/random.h>

/* ---------- FD table ---------- */
static fd_entry_t fd_table[FD_TABLE_SIZE];

/* ---------- Signal stubs ---------- */
/* We store signal actions but never deliver signals. */
#define MAX_SIGNALS 64
static uint8_t sig_actions[MAX_SIGNALS]; /* 0 = default, 1 = set */

void syscall_init(void) {
    memset(fd_table, 0, sizeof(fd_table));
    memset(sig_actions, 0, sizeof(sig_actions));

    /* Pre-open stdin/stdout/stderr */
    fd_table[0] = (fd_entry_t){ .type = FD_STDIO, .host_fd = STDIN_FILENO };
    fd_table[1] = (fd_entry_t){ .type = FD_STDIO, .host_fd = STDOUT_FILENO };
    fd_table[2] = (fd_entry_t){ .type = FD_STDIO, .host_fd = STDERR_FILENO };
}

/* ---------- FD helpers ---------- */

/* Allocate the lowest available FD. Returns -1 if table is full. */
static int fd_alloc(int type, int host_fd) {
    for (int i = 0; i < FD_TABLE_SIZE; i++) {
        if (fd_table[i].type == FD_CLOSED) {
            fd_table[i].type = type;
            fd_table[i].host_fd = host_fd;
            return i;
        }
    }
    return -1;
}

/* Allocate a specific FD slot. Returns -1 if out of range. */
static int fd_alloc_at(int fd, int type, int host_fd) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -1;
    if (fd_table[fd].type != FD_CLOSED) {
        close(fd_table[fd].host_fd);
    }
    fd_table[fd].type = type;
    fd_table[fd].host_fd = host_fd;
    return fd;
}

/* Look up a guest FD. Returns host FD or -1 if invalid. */
static int fd_to_host(int guest_fd) {
    if (guest_fd < 0 || guest_fd >= FD_TABLE_SIZE)
        return -1;
    if (fd_table[guest_fd].type == FD_CLOSED)
        return -1;
    return fd_table[guest_fd].host_fd;
}

/* ---------- Linux errno translation ---------- */

/* Convert macOS errno to Linux errno.
 * Most values are identical on both platforms; handle known differences. */
static int64_t linux_errno(void) {
    /* Most errno values are the same on macOS and Linux for basic ones.
     * Return as negative value for Linux syscall convention. */
    return -(int64_t)errno;
}

/* ---------- Linux open flags translation ---------- */
static int translate_open_flags(int linux_flags) {
    int flags = 0;
    int accmode = linux_flags & 3;
    if (accmode == LINUX_O_RDONLY) flags |= O_RDONLY;
    else if (accmode == LINUX_O_WRONLY) flags |= O_WRONLY;
    else if (accmode == LINUX_O_RDWR) flags |= O_RDWR;

    if (linux_flags & LINUX_O_CREAT)    flags |= O_CREAT;
    if (linux_flags & LINUX_O_EXCL)     flags |= O_EXCL;
    if (linux_flags & LINUX_O_TRUNC)    flags |= O_TRUNC;
    if (linux_flags & LINUX_O_APPEND)   flags |= O_APPEND;
    if (linux_flags & LINUX_O_NONBLOCK) flags |= O_NONBLOCK;
    if (linux_flags & LINUX_O_CLOEXEC)  flags |= O_CLOEXEC;
    if (linux_flags & LINUX_O_DIRECTORY) flags |= O_DIRECTORY;

    return flags;
}

/* ---------- stat translation ---------- */
static void translate_stat(const struct stat *mac, linux_stat_t *lin) {
    memset(lin, 0, sizeof(*lin));
    lin->st_dev     = mac->st_dev;
    lin->st_ino     = mac->st_ino;
    lin->st_mode    = mac->st_mode;
    lin->st_nlink   = (uint32_t)mac->st_nlink;
    lin->st_uid     = mac->st_uid;
    lin->st_gid     = mac->st_gid;
    lin->st_rdev    = mac->st_rdev;
    lin->st_size    = mac->st_size;
    lin->st_blksize = (int32_t)mac->st_blksize;
    lin->st_blocks  = mac->st_blocks;
    lin->st_atime_sec  = mac->st_atimespec.tv_sec;
    lin->st_atime_nsec = mac->st_atimespec.tv_nsec;
    lin->st_mtime_sec  = mac->st_mtimespec.tv_sec;
    lin->st_mtime_nsec = mac->st_mtimespec.tv_nsec;
    lin->st_ctime_sec  = mac->st_ctimespec.tv_sec;
    lin->st_ctime_nsec = mac->st_ctimespec.tv_nsec;
}

/* ---------- Individual syscall handlers ---------- */

static int64_t sys_write(guest_t *g, int fd, uint64_t buf_gva, uint64_t count) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    ssize_t ret = write(host_fd, buf, count);
    return ret < 0 ? linux_errno() : ret;
}

static int64_t sys_read(guest_t *g, int fd, uint64_t buf_gva, uint64_t count) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    ssize_t ret = read(host_fd, buf, count);
    return ret < 0 ? linux_errno() : ret;
}

static int64_t sys_writev(guest_t *g, int fd, uint64_t iov_gva, int iovcnt) {
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
    return ret < 0 ? linux_errno() : ret;
}

static int64_t sys_openat(guest_t *g, int dirfd, uint64_t path_gva,
                          int linux_flags, int mode) {
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    int host_dirfd;
    if (dirfd == LINUX_AT_FDCWD) {
        host_dirfd = AT_FDCWD;
    } else {
        host_dirfd = fd_to_host(dirfd);
        if (host_dirfd < 0) return -LINUX_EBADF;
    }

    int flags = translate_open_flags(linux_flags);
    int host_fd = openat(host_dirfd, path, flags, mode);
    if (host_fd < 0) return linux_errno();

    int type = (linux_flags & LINUX_O_DIRECTORY) ? FD_DIR : FD_REGULAR;
    int guest_fd = fd_alloc(type, host_fd);
    if (guest_fd < 0) {
        close(host_fd);
        return -LINUX_ENOMEM;
    }

    return guest_fd;
}

static int64_t sys_close(int fd) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -LINUX_EBADF;
    if (fd_table[fd].type == FD_CLOSED) return -LINUX_EBADF;

    /* Don't actually close stdin/stdout/stderr on the host */
    if (fd_table[fd].type != FD_STDIO) {
        close(fd_table[fd].host_fd);
    }
    fd_table[fd].type = FD_CLOSED;
    fd_table[fd].host_fd = -1;
    return 0;
}

static int64_t sys_fstat(guest_t *g, int fd, uint64_t stat_gva) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    struct stat mac_st;
    if (fstat(host_fd, &mac_st) < 0) return linux_errno();

    linux_stat_t lin_st;
    translate_stat(&mac_st, &lin_st);

    if (guest_write(g, stat_gva, &lin_st, sizeof(lin_st)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

static int64_t sys_newfstatat(guest_t *g, int dirfd, uint64_t path_gva,
                              uint64_t stat_gva, int flags) {
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    int host_dirfd;
    if (dirfd == LINUX_AT_FDCWD) {
        host_dirfd = AT_FDCWD;
    } else {
        host_dirfd = fd_to_host(dirfd);
        if (host_dirfd < 0) return -LINUX_EBADF;
    }

    /* Translate Linux AT_* flags to macOS (they're the same values) */
    struct stat mac_st;
    if (fstatat(host_dirfd, path, &mac_st, flags) < 0)
        return linux_errno();

    linux_stat_t lin_st;
    translate_stat(&mac_st, &lin_st);

    if (guest_write(g, stat_gva, &lin_st, sizeof(lin_st)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

static int64_t sys_ioctl(int fd, uint64_t request) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Only stub TIOCGWINSZ for terminal queries */
    if (request == LINUX_TIOCGWINSZ) {
        return -LINUX_ENOTTY;
    }

    return -LINUX_ENOTTY;
}

static int64_t sys_uname(guest_t *g, uint64_t buf_gva) {
    linux_utsname_t uts;
    memset(&uts, 0, sizeof(uts));
    strncpy(uts.sysname, "Linux", LINUX_UTSNAME_LEN - 1);
    strncpy(uts.nodename, "hl", LINUX_UTSNAME_LEN - 1);
    strncpy(uts.release, "6.1.0", LINUX_UTSNAME_LEN - 1);
    strncpy(uts.version, "#1 SMP", LINUX_UTSNAME_LEN - 1);
    strncpy(uts.machine, "aarch64", LINUX_UTSNAME_LEN - 1);
    strncpy(uts.domainname, "(none)", LINUX_UTSNAME_LEN - 1);

    if (guest_write(g, buf_gva, &uts, sizeof(uts)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

static int64_t sys_brk(guest_t *g, uint64_t addr) {
    /* brk addresses as seen by the guest are IPA-based */
    uint64_t ipa_brk = guest_ipa(g, g->brk_current);
    uint64_t ipa_base = guest_ipa(g, g->brk_base);

    if (addr == 0) {
        return (int64_t)ipa_brk;
    }

    if (addr < ipa_base) {
        return (int64_t)ipa_brk;
    }

    /* Convert IPA back to offset for internal tracking */
    uint64_t new_off = addr - g->ipa_base;
    if (new_off >= g->guest_size) {
        return (int64_t)ipa_brk;
    }

    /* Zero new pages if growing */
    if (new_off > g->brk_current) {
        memset((uint8_t *)g->host_base + g->brk_current, 0,
               new_off - g->brk_current);
    }

    g->brk_current = new_off;
    return (int64_t)guest_ipa(g, g->brk_current);
}

static int64_t sys_mmap(guest_t *g, uint64_t addr, uint64_t length,
                        int prot, int flags, int fd, int64_t offset) {
    (void)prot; (void)fd; (void)offset;

    /* We only handle MAP_ANONYMOUS for now */
    if (!(flags & LINUX_MAP_ANONYMOUS)) {
        return -LINUX_ENOSYS;
    }

    /* Round length up to page size */
    length = (length + 4095) & ~4095ULL;

    uint64_t result_off;  /* Result as offset (0-based) */
    if (flags & LINUX_MAP_FIXED) {
        /* MAP_FIXED: addr is IPA-based, convert to offset */
        uint64_t off = addr - g->ipa_base;
        if (off + length > g->guest_size) return -LINUX_ENOMEM;
        result_off = off;
    } else {
        /* Bump allocator from mmap region */
        g->mmap_next = (g->mmap_next + 4095) & ~4095ULL;
        if (g->mmap_next + length > MMAP_END) return -LINUX_ENOMEM;
        result_off = g->mmap_next;
        g->mmap_next += length;
    }

    /* Zero the mapped region */
    memset((uint8_t *)g->host_base + result_off, 0, length);

    /* Return IPA-based address to guest */
    return (int64_t)guest_ipa(g, result_off);
}

static int64_t sys_clock_gettime(guest_t *g, int clockid, uint64_t tp_gva) {
    struct timespec ts;
    /* Map Linux clock IDs to macOS (CLOCK_REALTIME=0, CLOCK_MONOTONIC=1 on both) */
    if (clock_gettime(clockid, &ts) < 0)
        return linux_errno();

    linux_timespec_t lts;
    lts.tv_sec = ts.tv_sec;
    lts.tv_nsec = ts.tv_nsec;

    if (guest_write(g, tp_gva, &lts, sizeof(lts)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

static int64_t sys_getrandom(guest_t *g, uint64_t buf_gva, uint64_t buflen,
                             unsigned int flags) {
    (void)flags;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    /* getentropy() works in chunks of 256 bytes max */
    uint8_t *p = buf;
    size_t remaining = buflen;
    while (remaining > 0) {
        size_t chunk = remaining > 256 ? 256 : remaining;
        if (getentropy(p, chunk) != 0)
            return linux_errno();
        p += chunk;
        remaining -= chunk;
    }

    return (int64_t)buflen;
}

static int64_t sys_getcwd(guest_t *g, uint64_t buf_gva, uint64_t size) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd)))
        return linux_errno();

    size_t len = strlen(cwd) + 1;
    if (len > size)
        return -LINUX_ERANGE;

    if (guest_write(g, buf_gva, cwd, len) < 0)
        return -LINUX_EFAULT;

    return (int64_t)len;
}

static int64_t sys_dup(int oldfd) {
    int host_fd = fd_to_host(oldfd);
    if (host_fd < 0) return -LINUX_EBADF;

    int new_host_fd = dup(host_fd);
    if (new_host_fd < 0) return linux_errno();

    int guest_fd = fd_alloc(fd_table[oldfd].type, new_host_fd);
    if (guest_fd < 0) {
        close(new_host_fd);
        return -LINUX_ENOMEM;
    }

    return guest_fd;
}

static int64_t sys_dup3(int oldfd, int newfd, int flags) {
    (void)flags;
    int host_fd = fd_to_host(oldfd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (newfd < 0 || newfd >= FD_TABLE_SIZE) return -LINUX_EBADF;

    int new_host_fd = dup(host_fd);
    if (new_host_fd < 0) return linux_errno();

    fd_alloc_at(newfd, fd_table[oldfd].type, new_host_fd);
    return newfd;
}

static int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Linux F_DUPFD=0, F_GETFD=1, F_SETFD=2, F_GETFL=3, F_SETFL=4 */
    switch (cmd) {
    case 1: /* F_GETFD */
        return fcntl(host_fd, F_GETFD);
    case 2: /* F_SETFD */
        return fcntl(host_fd, F_SETFD, (int)arg);
    case 3: /* F_GETFL */
        return fcntl(host_fd, F_GETFL);
    case 4: /* F_SETFL */
        return fcntl(host_fd, F_SETFL, (int)arg);
    default:
        return -LINUX_EINVAL;
    }
}

static int64_t sys_faccessat(guest_t *g, int dirfd, uint64_t path_gva,
                             int mode, int flags) {
    (void)flags;
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    int host_dirfd;
    if (dirfd == LINUX_AT_FDCWD) {
        host_dirfd = AT_FDCWD;
    } else {
        host_dirfd = fd_to_host(dirfd);
        if (host_dirfd < 0) return -LINUX_EBADF;
    }

    if (faccessat(host_dirfd, path, mode, 0) < 0)
        return linux_errno();

    return 0;
}

static int64_t sys_chdir(guest_t *g, uint64_t path_gva) {
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    if (chdir(path) < 0)
        return linux_errno();

    return 0;
}

static int64_t sys_pipe2(guest_t *g, uint64_t fds_gva, int flags) {
    (void)flags;
    int host_fds[2];
    if (pipe(host_fds) < 0)
        return linux_errno();

    int guest_fds[2];
    guest_fds[0] = fd_alloc(FD_REGULAR, host_fds[0]);
    guest_fds[1] = fd_alloc(FD_REGULAR, host_fds[1]);

    if (guest_fds[0] < 0 || guest_fds[1] < 0) {
        close(host_fds[0]);
        close(host_fds[1]);
        return -LINUX_ENOMEM;
    }

    int32_t fds[2] = { guest_fds[0], guest_fds[1] };
    if (guest_write(g, fds_gva, fds, sizeof(fds)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

/* Linux dirent64 layout */
typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    /* char d_name[] follows */
} __attribute__((packed)) linux_dirent64_t;

static int64_t sys_getdents64(guest_t *g, int fd, uint64_t buf_gva,
                              uint64_t count) {
    (void)g; (void)buf_gva; (void)count;

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* TODO: implement full getdents64 for coreutils support.
     * For now, return 0 (end of directory). */
    return 0;
}

static int64_t sys_lseek(int fd, int64_t offset, int whence) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    off_t ret = lseek(host_fd, offset, whence);
    return ret < 0 ? linux_errno() : (int64_t)ret;
}

static int64_t sys_readlinkat(guest_t *g, int dirfd, uint64_t path_gva,
                              uint64_t buf_gva, uint64_t bufsiz) {
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    int host_dirfd;
    if (dirfd == LINUX_AT_FDCWD) {
        host_dirfd = AT_FDCWD;
    } else {
        host_dirfd = fd_to_host(dirfd);
        if (host_dirfd < 0) return -LINUX_EBADF;
    }

    char link[4096];
    ssize_t len = readlinkat(host_dirfd, path, link, sizeof(link) - 1);
    if (len < 0) return linux_errno();

    size_t copy_len = (size_t)len < bufsiz ? (size_t)len : bufsiz;
    if (guest_write(g, buf_gva, link, copy_len) < 0)
        return -LINUX_EFAULT;

    return (int64_t)copy_len;
}

/* ---------- Main dispatch ---------- */

int syscall_dispatch(hv_vcpu_t vcpu, guest_t *g, int *exit_code, int verbose) {
    uint64_t x0, x1, x2, x3, x4, x5, x8;

    hv_vcpu_get_reg(vcpu, HV_REG_X8, &x8);
    hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0);
    hv_vcpu_get_reg(vcpu, HV_REG_X1, &x1);
    hv_vcpu_get_reg(vcpu, HV_REG_X2, &x2);
    hv_vcpu_get_reg(vcpu, HV_REG_X3, &x3);
    hv_vcpu_get_reg(vcpu, HV_REG_X4, &x4);
    hv_vcpu_get_reg(vcpu, HV_REG_X5, &x5);

    if (verbose) {
        fprintf(stderr, "hl: syscall %llu(0x%llx, 0x%llx, 0x%llx, 0x%llx, "
                "0x%llx, 0x%llx)\n",
                (unsigned long long)x8,
                (unsigned long long)x0, (unsigned long long)x1,
                (unsigned long long)x2, (unsigned long long)x3,
                (unsigned long long)x4, (unsigned long long)x5);
    }

    int64_t result = 0;
    int should_exit = 0;

    switch ((int)x8) {
    /* ---- Tier 1: assembly hello world ---- */
    case SYS_write:
        result = sys_write(g, (int)x0, x1, x2);
        break;
    case SYS_exit:
        *exit_code = (int)x0;
        should_exit = 1;
        break;
    case SYS_exit_group:
        *exit_code = (int)x0;
        should_exit = 1;
        break;

    /* ---- Tier 2: musl static hello world ---- */
    case SYS_read:
        result = sys_read(g, (int)x0, x1, x2);
        break;
    case SYS_openat:
        result = sys_openat(g, (int)x0, x1, (int)x2, (int)x3);
        break;
    case SYS_close:
        result = sys_close((int)x0);
        break;
    case SYS_writev:
        result = sys_writev(g, (int)x0, x1, (int)x2);
        break;
    case SYS_ioctl:
        result = sys_ioctl((int)x0, x1);
        break;
    case SYS_fstat:
        result = sys_fstat(g, (int)x0, x1);
        break;
    case SYS_newfstatat:
        result = sys_newfstatat(g, (int)x0, x1, x2, (int)x3);
        break;
    case SYS_set_tid_address:
        result = 1; /* return PID=1 */
        break;
    case SYS_clock_gettime:
        result = sys_clock_gettime(g, (int)x0, x1);
        break;
    case SYS_rt_sigaction:
        /* Stub: pretend we stored the action */
        if ((int)x0 > 0 && (int)x0 < MAX_SIGNALS)
            sig_actions[(int)x0] = 1;
        result = 0;
        break;
    case SYS_rt_sigprocmask:
        /* Stub: ignore signal mask changes */
        result = 0;
        break;
    case SYS_uname:
        result = sys_uname(g, x0);
        break;
    case SYS_getpid:
        result = 1;
        break;
    case SYS_gettid:
        result = 1;
        break;
    case SYS_brk:
        result = sys_brk(g, x0);
        break;
    case SYS_mmap:
        result = sys_mmap(g, x0, x1, (int)x2, (int)x3, (int)x4, (int64_t)x5);
        break;
    case SYS_munmap:
        /* Stub: we don't actually unmap */
        result = 0;
        break;
    case SYS_mprotect:
        /* Stub: we don't enforce per-page protection */
        result = 0;
        break;
    case SYS_getrandom:
        result = sys_getrandom(g, x0, x1, (unsigned int)x2);
        break;

    /* ---- Tier 3: coreutils ---- */
    case SYS_getcwd:
        result = sys_getcwd(g, x0, x1);
        break;
    case SYS_dup:
        result = sys_dup((int)x0);
        break;
    case SYS_dup3:
        result = sys_dup3((int)x0, (int)x1, (int)x2);
        break;
    case SYS_fcntl:
        result = sys_fcntl((int)x0, (int)x1, x2);
        break;
    case SYS_faccessat:
        result = sys_faccessat(g, (int)x0, x1, (int)x2, (int)x3);
        break;
    case SYS_chdir:
        result = sys_chdir(g, x0);
        break;
    case SYS_pipe2:
        result = sys_pipe2(g, x0, (int)x1);
        break;
    case SYS_getdents64:
        result = sys_getdents64(g, (int)x0, x1, x2);
        break;
    case SYS_lseek:
        result = sys_lseek((int)x0, (int64_t)x1, (int)x2);
        break;
    case SYS_readlinkat:
        result = sys_readlinkat(g, (int)x0, x1, x2, x3);
        break;

    default:
        if (verbose) {
            fprintf(stderr, "hl: unimplemented syscall %llu "
                    "(x0=0x%llx, x1=0x%llx, x2=0x%llx, x3=0x%llx, "
                    "x4=0x%llx, x5=0x%llx)\n",
                    (unsigned long long)x8,
                    (unsigned long long)x0, (unsigned long long)x1,
                    (unsigned long long)x2, (unsigned long long)x3,
                    (unsigned long long)x4, (unsigned long long)x5);
        }
        result = -LINUX_ENOSYS;
        break;
    }

    if (!should_exit) {
        /* Write result back to X0 */
        hv_vcpu_set_reg(vcpu, HV_REG_X0, (uint64_t)result);
    }

    return should_exit;
}

/* ---------- Linux stack builder ---------- */

/* Push a uint64_t onto the stack (growing downward) */
static void push_u64(guest_t *g, uint64_t *sp, uint64_t val) {
    *sp -= 8;
    guest_write(g, *sp, &val, 8);
}

/* Write a string to guest memory at the given address, return length+1 */
static size_t write_str(guest_t *g, uint64_t gva, const char *s) {
    size_t len = strlen(s) + 1;
    guest_write(g, gva, s, len);
    return len;
}

uint64_t build_linux_stack(guest_t *g, uint64_t stack_top,
                           int argc, const char **argv,
                           const elf_info_t *elf_info) {
    /*
     * Linux initial stack layout (growing from high to low):
     *   [ 16 random bytes for AT_RANDOM ]
     *   [ "aarch64\0" for AT_PLATFORM ]
     *   [ environment strings ]
     *   [ argument strings ]
     *   [ padding to 16-byte alignment ]
     *   [ AT_NULL (0, 0) ]
     *   [ auxv entries (key, value) pairs ]
     *   [ NULL (end of envp) ]
     *   [ envp[0], envp[1], ... ]  -- none for now
     *   [ NULL (end of argv) ]
     *   [ argv[argc-1] ... argv[0] ]
     *   [ argc ]                    <-- SP points here
     */

    /* Phase 1: Write strings and random data at the top of the stack.
     * We work downward from stack_top. */
    uint64_t str_ptr = stack_top;

    /* AT_RANDOM: 16 random bytes */
    str_ptr -= 16;
    uint64_t random_ptr = str_ptr;
    uint8_t random_bytes[16];
    getentropy(random_bytes, 16);
    guest_write(g, random_ptr, random_bytes, 16);

    /* AT_PLATFORM: "aarch64\0" */
    str_ptr -= 8;  /* strlen("aarch64") + 1 */
    uint64_t platform_ptr = str_ptr;
    write_str(g, platform_ptr, "aarch64");

    /* Argument strings (written backward so argv[0] is at lowest addr) */
    uint64_t arg_ptrs[256];
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        str_ptr -= len;
        arg_ptrs[i] = str_ptr;
        write_str(g, str_ptr, argv[i]);
    }

    /* Phase 2: Build the structured part of the stack.
     * Align str_ptr down to 16 bytes first. */
    str_ptr &= ~15ULL;
    uint64_t sp = str_ptr;

    /* Auxv entries (pushed in reverse order since stack grows down).
     * We push AT_NULL last (first in stack order). */
    push_u64(g, &sp, 0); push_u64(g, &sp, AT_NULL);

    push_u64(g, &sp, platform_ptr); push_u64(g, &sp, AT_PLATFORM);
    push_u64(g, &sp, random_ptr); push_u64(g, &sp, AT_RANDOM);
    push_u64(g, &sp, 100); push_u64(g, &sp, AT_CLKTCK);

    /* HWCAP: advertise basic aarch64 features
     * Bit 0=FP, Bit 1=ASIMD, Bit 3=AES, Bit 4=PMULL, etc. */
    push_u64(g, &sp, 0xFF); push_u64(g, &sp, AT_HWCAP);

    push_u64(g, &sp, 0); push_u64(g, &sp, AT_EGID);
    push_u64(g, &sp, 0); push_u64(g, &sp, AT_GID);
    push_u64(g, &sp, 0); push_u64(g, &sp, AT_EUID);
    push_u64(g, &sp, 0); push_u64(g, &sp, AT_UID);

    push_u64(g, &sp, elf_info->entry); push_u64(g, &sp, AT_ENTRY);
    push_u64(g, &sp, elf_info->phnum); push_u64(g, &sp, AT_PHNUM);
    push_u64(g, &sp, elf_info->phentsize); push_u64(g, &sp, AT_PHENT);
    push_u64(g, &sp, elf_info->phdr_gpa); push_u64(g, &sp, AT_PHDR);
    push_u64(g, &sp, 4096); push_u64(g, &sp, AT_PAGESZ);

    /* envp: no environment variables for now */
    push_u64(g, &sp, 0);  /* NULL terminator */

    /* argv: NULL terminator, then pointers in reverse order */
    push_u64(g, &sp, 0);  /* NULL terminator */
    for (int i = argc - 1; i >= 0; i--) {
        push_u64(g, &sp, arg_ptrs[i]);
    }

    /* argc */
    push_u64(g, &sp, (uint64_t)argc);

    /* SP must be 16-byte aligned per AArch64 ABI */
    sp &= ~15ULL;

    return sp;
}
