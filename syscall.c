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
#include "syscall_internal.h"
#include "syscall_proc.h"

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
#include <sys/time.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <poll.h>
#include <termios.h>
#include <sched.h>
#include <mach/mach.h>

/* ---------- FD table ---------- */
fd_entry_t fd_table[FD_TABLE_SIZE];

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
int fd_alloc(int type, int host_fd) {
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
int fd_alloc_at(int fd, int type, int host_fd) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -1;
    if (fd_table[fd].type != FD_CLOSED) {
        close(fd_table[fd].host_fd);
    }
    fd_table[fd].type = type;
    fd_table[fd].host_fd = host_fd;
    return fd;
}

/* Look up a guest FD. Returns host FD or -1 if invalid. */
int fd_to_host(int guest_fd) {
    if (guest_fd < 0 || guest_fd >= FD_TABLE_SIZE)
        return -1;
    if (fd_table[guest_fd].type == FD_CLOSED)
        return -1;
    return fd_table[guest_fd].host_fd;
}

/* ---------- Linux errno translation ---------- */

/* Convert macOS errno to the equivalent Linux errno value.
 * macOS and Linux errno values diverge starting around errno 35.
 * Returns the negative Linux errno for direct use as a syscall return. */
int64_t linux_errno(void) {
    int e = errno;

    /* Values 1-34 are mostly identical between macOS and Linux, with
     * specific exceptions handled below. For values >= 35 the mapping
     * diverges significantly — a translation table is required. */
    switch (e) {
    /* macOS-specific values that differ from Linux */
    case EAGAIN:       return -LINUX_EAGAIN;       /* mac 35 → linux 11 */
    case EBUSY:        return -LINUX_EBUSY;        /* mac 16 → linux 16 (same) */
    case EEXIST:       return -LINUX_EEXIST;       /* mac 17 → linux 17 (same) */
    case EXDEV:        return -LINUX_EXDEV;        /* mac 18 → linux 18 (same) */
    case ENODEV:       return -LINUX_ENODEV;       /* mac 19 → linux 19 (same) */
    case EISDIR:       return -LINUX_EISDIR;       /* mac 21 → linux 21 (same) */
    case ENFILE:       return -LINUX_ENFILE;       /* mac 23 → linux 23 (same) */
    case EMFILE:       return -LINUX_EMFILE;       /* mac 24 → linux 24 (same) */
    case ETXTBSY:      return -LINUX_ETXTBSY;      /* mac 26 → linux 26 (same) */
    case EFBIG:        return -LINUX_EFBIG;        /* mac 27 → linux 27 (same) */
    case ENOSPC:       return -LINUX_ENOSPC;       /* mac 28 → linux 28 (same) */
    case ESPIPE:       return -LINUX_ESPIPE;       /* mac 29 → linux 29 (same) */
    case EROFS:        return -LINUX_EROFS;        /* mac 30 → linux 30 (same) */
    case EMLINK:       return -LINUX_EMLINK;       /* mac 31 → linux 31 (same) */
    case EPIPE:        return -LINUX_EPIPE;        /* mac 32 → linux 32 (same) */
    case ERANGE:       return -LINUX_ERANGE;       /* mac 34 → linux 34 (same) */
    case EDEADLK:      return -LINUX_EDEADLK;      /* mac 11 → linux 35 */
    case ENAMETOOLONG:  return -LINUX_ENAMETOOLONG;  /* mac 63 → linux 36 */
    case ENOLCK:       return -LINUX_ENOLCK;       /* mac 77 → linux 37 */
    case ENOSYS:       return -LINUX_ENOSYS;       /* mac 78 → linux 38 */
    case ENOTEMPTY:    return -LINUX_ENOTEMPTY;    /* mac 66 → linux 39 */
    case ELOOP:        return -LINUX_ELOOP;        /* mac 62 → linux 40 */
    case ENOPROTOOPT:  return -LINUX_ENOPROTOOPT;  /* mac 42 → linux 92 */
    case EOPNOTSUPP:   return -LINUX_EOPNOTSUPP;   /* mac 45 → linux 95 */
    case EOVERFLOW:    return -LINUX_EOVERFLOW;    /* mac 84 → linux 75 */
    default:
        /* For errno values 1-34 not listed above, numeric values match.
         * For unmapped values, pass through (best effort). */
        if (e >= 1 && e <= 34) return -(int64_t)e;
        return -LINUX_EINVAL;  /* Fallback for truly unknown errno values */
    }
}

/* ---------- Linux AT_* flags translation ---------- */

/* Translate Linux AT_* flags to macOS equivalents.
 * Linux and macOS use different values for AT_SYMLINK_NOFOLLOW etc. */
int translate_at_flags(int linux_flags) {
    int mac_flags = 0;
    if (linux_flags & LINUX_AT_SYMLINK_NOFOLLOW)
        mac_flags |= AT_SYMLINK_NOFOLLOW;
    if (linux_flags & LINUX_AT_SYMLINK_FOLLOW)
        mac_flags |= AT_SYMLINK_FOLLOW;
    if (linux_flags & LINUX_AT_REMOVEDIR)
        mac_flags |= AT_REMOVEDIR;
    /* AT_EMPTY_PATH not supported on macOS */
    return mac_flags;
}

/* Resolve dirfd: translate LINUX_AT_FDCWD and guest FDs */
int resolve_dirfd(int dirfd) {
    if (dirfd == LINUX_AT_FDCWD) return AT_FDCWD;
    return fd_to_host(dirfd);
}

/* ---------- Linux open flags translation ---------- */
int translate_open_flags(int linux_flags) {
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
    if (linux_flags & LINUX_O_NOFOLLOW)  flags |= O_NOFOLLOW;
    if (linux_flags & LINUX_O_CLOEXEC)   flags |= O_CLOEXEC;
    if (linux_flags & LINUX_O_DIRECTORY) flags |= O_DIRECTORY;
    /* LINUX_O_LARGEFILE: ignored — macOS always uses 64-bit offsets */
    /* LINUX_O_DIRECT: ignored — no O_DIRECT equivalent on macOS */

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

static int64_t sys_pread64(guest_t *g, int fd, uint64_t buf_gva,
                            uint64_t count, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    ssize_t ret = pread(host_fd, buf, count, offset);
    return ret < 0 ? linux_errno() : ret;
}

static int64_t sys_pwrite64(guest_t *g, int fd, uint64_t buf_gva,
                             uint64_t count, int64_t offset) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    void *buf = guest_ptr(g, buf_gva);
    if (!buf) return -LINUX_EFAULT;

    ssize_t ret = pwrite(host_fd, buf, count, offset);
    return ret < 0 ? linux_errno() : ret;
}

static int64_t sys_readv(guest_t *g, int fd, uint64_t iov_gva, int iovcnt) {
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

    int is_dir = (linux_flags & LINUX_O_DIRECTORY) != 0;

    /* If O_DIRECTORY wasn't explicitly set, check via fstat whether
     * the opened fd is actually a directory (musl's opendir path) */
    if (!is_dir) {
        struct stat st;
        if (fstat(host_fd, &st) == 0 && S_ISDIR(st.st_mode))
            is_dir = 1;
    }

    int type = is_dir ? FD_DIR : FD_REGULAR;
    int guest_fd = fd_alloc(type, host_fd);
    if (guest_fd < 0) {
        close(host_fd);
        return -LINUX_ENOMEM;
    }
    fd_table[guest_fd].linux_flags = linux_flags;

    /* For directories, create a DIR* for subsequent getdents64 calls */
    if (is_dir) {
        int dup_fd = dup(host_fd);
        if (dup_fd >= 0) {
            DIR *dir = fdopendir(dup_fd);
            if (dir) {
                fd_table[guest_fd].dir = dir;
            } else {
                close(dup_fd);
            }
        }
    }

    return guest_fd;
}

static int64_t sys_close(int fd) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -LINUX_EBADF;
    if (fd_table[fd].type == FD_CLOSED) return -LINUX_EBADF;

    /* Close DIR* if this was a directory fd */
    if (fd_table[fd].dir) {
        closedir((DIR *)fd_table[fd].dir);
        fd_table[fd].dir = NULL;
    }

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

    /* Translate Linux AT_* flags to macOS equivalents */
    int mac_flags = translate_at_flags(flags);
    struct stat mac_st;
    if (fstatat(host_dirfd, path, &mac_st, mac_flags) < 0)
        return linux_errno();

    linux_stat_t lin_st;
    translate_stat(&mac_st, &lin_st);

    if (guest_write(g, stat_gva, &lin_st, sizeof(lin_st)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

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

static int64_t sys_ioctl(guest_t *g, int fd, uint64_t request, uint64_t arg) {
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

    case LINUX_TCSETS: {
        /* Set terminal attributes */
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
        if (tcsetattr(host_fd, TCSANOW, &t) < 0)
            return linux_errno();
        return 0;
    }

    default:
        return -LINUX_ENOTTY;
    }
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

    /* Extend page tables if brk grows beyond currently-mapped region.
     * The brk region is initially mapped up to MMAP_BASE; if it grows
     * past that, we need to extend dynamically. */
    uint64_t brk_pt_end = (g->brk_current + BLOCK_2MB - 1) & ~(BLOCK_2MB - 1);
    if (brk_pt_end < MMAP_BASE) brk_pt_end = MMAP_BASE;
    if (new_off > brk_pt_end) {
        uint64_t new_end = (new_off + BLOCK_2MB - 1) & ~(BLOCK_2MB - 1);
        if (guest_extend_page_tables(g, brk_pt_end, new_end, MEM_PERM_RW) < 0)
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
    (void)prot;

    int is_anon = (flags & LINUX_MAP_ANONYMOUS) != 0;

    /* We handle MAP_ANONYMOUS and file-backed MAP_PRIVATE */
    if (!is_anon && !(flags & LINUX_MAP_PRIVATE)) {
        return -LINUX_ENOSYS;  /* MAP_SHARED not supported */
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

    /* Extend page tables if we've grown beyond the currently-mapped region */
    if (result_off + length > g->mmap_end) {
        uint64_t new_end = (result_off + length + BLOCK_2MB - 1)
                           & ~(BLOCK_2MB - 1);
        if (new_end > MMAP_END) new_end = MMAP_END;
        if (guest_extend_page_tables(g, g->mmap_end, new_end, MEM_PERM_RW) < 0)
            return -LINUX_ENOMEM;
        g->mmap_end = new_end;
    }

    /* Zero the mapped region */
    memset((uint8_t *)g->host_base + result_off, 0, length);

    /* For file-backed mmap, read file contents into the region */
    if (!is_anon && fd >= 0) {
        int host_fd = fd_to_host(fd);
        if (host_fd < 0) return -LINUX_EBADF;
        pread(host_fd, (uint8_t *)g->host_base + result_off, length, offset);
    }

    /* Return IPA-based address to guest */
    return (int64_t)guest_ipa(g, result_off);
}

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

static int64_t sys_clock_gettime(guest_t *g, int clockid, uint64_t tp_gva) {
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

static int64_t sys_dup3(int oldfd, int newfd, int linux_flags) {
    if (oldfd < 0 || oldfd >= FD_TABLE_SIZE) return -LINUX_EBADF;
    int host_fd = fd_to_host(oldfd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (newfd < 0 || newfd >= FD_TABLE_SIZE) return -LINUX_EBADF;

    int new_host_fd = dup(host_fd);
    if (new_host_fd < 0) return linux_errno();

    fd_alloc_at(newfd, fd_table[oldfd].type, new_host_fd);
    /* O_CLOEXEC in dup3 flags sets CLOEXEC on new fd */
    fd_table[newfd].linux_flags = (linux_flags & LINUX_O_CLOEXEC);
    return newfd;
}

static int64_t sys_fcntl(int fd, int cmd, uint64_t arg) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -LINUX_EBADF;
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Linux F_DUPFD=0, F_GETFD=1, F_SETFD=2, F_GETFL=3, F_SETFL=4 */
    switch (cmd) {
    case 0: { /* F_DUPFD */
        int new_host = dup(host_fd);
        if (new_host < 0) return linux_errno();
        int gfd = fd_alloc(fd_table[fd].type, new_host);
        if (gfd < 0) { close(new_host); return -LINUX_ENOMEM; }
        fd_table[gfd].linux_flags = fd_table[fd].linux_flags & ~LINUX_O_CLOEXEC;
        return gfd;
    }
    case 1: /* F_GETFD */
        return (fd_table[fd].linux_flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
    case 2: /* F_SETFD */
        if ((int)arg & LINUX_FD_CLOEXEC)
            fd_table[fd].linux_flags |= LINUX_O_CLOEXEC;
        else
            fd_table[fd].linux_flags &= ~LINUX_O_CLOEXEC;
        return 0;
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

static int64_t sys_pipe2(guest_t *g, uint64_t fds_gva, int linux_flags) {
    int host_fds[2];
    if (pipe(host_fds) < 0)
        return linux_errno();

    int guest_fds[2];
    guest_fds[0] = fd_alloc(FD_PIPE, host_fds[0]);
    guest_fds[1] = fd_alloc(FD_PIPE, host_fds[1]);

    if (guest_fds[0] < 0 || guest_fds[1] < 0) {
        close(host_fds[0]);
        close(host_fds[1]);
        return -LINUX_ENOMEM;
    }

    /* Propagate O_CLOEXEC if set in flags */
    fd_table[guest_fds[0]].linux_flags = linux_flags & LINUX_O_CLOEXEC;
    fd_table[guest_fds[1]].linux_flags = linux_flags & LINUX_O_CLOEXEC;

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

/* getdents64: read directory entries from a guest directory fd.
 * Uses the persistent DIR* stored in fd_table (created by openat). */
static int64_t sys_getdents64(guest_t *g, int fd, uint64_t buf_gva,
                              uint64_t count) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -LINUX_EBADF;
    if (fd_table[fd].type == FD_CLOSED) return -LINUX_EBADF;

    DIR *dir = (DIR *)fd_table[fd].dir;
    if (!dir) return -LINUX_ENOTDIR;

    uint8_t *guest_buf = guest_ptr(g, buf_gva);
    if (!guest_buf) return -LINUX_EFAULT;

    size_t guest_pos = 0;
    struct dirent *de;

    while ((de = readdir(dir)) != NULL) {
        size_t name_len = strlen(de->d_name);
        /* Linux dirent64: 19-byte header + name + null, padded to 8 */
        size_t reclen = (19 + name_len + 1 + 7) & ~7ULL;

        if (guest_pos + reclen > count) {
            /* Unread this entry for the next getdents64 call */
            seekdir(dir, telldir(dir) - 1);
            break;
        }

        linux_dirent64_t lde;
        lde.d_ino = de->d_ino;
        lde.d_off = telldir(dir);
        lde.d_reclen = (uint16_t)reclen;
        lde.d_type = de->d_type;

        memcpy(guest_buf + guest_pos, &lde, sizeof(lde));
        memcpy(guest_buf + guest_pos + 19, de->d_name, name_len + 1);
        size_t pad_start = 19 + name_len + 1;
        if (pad_start < reclen)
            memset(guest_buf + guest_pos + pad_start, 0, reclen - pad_start);

        guest_pos += reclen;
    }

    return (int64_t)guest_pos;
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

static int64_t sys_unlinkat(guest_t *g, int dirfd, uint64_t path_gva, int flags) {
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

    int host_flags = translate_at_flags(flags);
    if (unlinkat(host_dirfd, path, host_flags) < 0)
        return linux_errno();

    return 0;
}

static int64_t sys_mkdirat(guest_t *g, int dirfd, uint64_t path_gva, int mode) {
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

    if (mkdirat(host_dirfd, path, (mode_t)mode) < 0)
        return linux_errno();

    return 0;
}

static int64_t sys_renameat2(guest_t *g, int olddirfd, uint64_t oldpath_gva,
                              int newdirfd, uint64_t newpath_gva, int flags) {
    (void)flags; /* Linux RENAME_NOREPLACE etc. — ignore for now */
    char oldpath[4096], newpath[4096];
    if (guest_read_str(g, oldpath_gva, oldpath, sizeof(oldpath)) < 0)
        return -LINUX_EFAULT;
    if (guest_read_str(g, newpath_gva, newpath, sizeof(newpath)) < 0)
        return -LINUX_EFAULT;

    int host_olddir = (olddirfd == LINUX_AT_FDCWD) ? AT_FDCWD : fd_to_host(olddirfd);
    int host_newdir = (newdirfd == LINUX_AT_FDCWD) ? AT_FDCWD : fd_to_host(newdirfd);
    if (host_olddir < 0 && olddirfd != LINUX_AT_FDCWD) return -LINUX_EBADF;
    if (host_newdir < 0 && newdirfd != LINUX_AT_FDCWD) return -LINUX_EBADF;

    if (renameat(host_olddir, oldpath, host_newdir, newpath) < 0)
        return linux_errno();

    return 0;
}

static int64_t sys_nanosleep(guest_t *g, uint64_t req_gva, uint64_t rem_gva) {
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

static int64_t sys_clock_nanosleep(guest_t *g, int clockid, int flags,
                                    uint64_t req_gva, uint64_t rem_gva) {
    (void)flags; /* TIMER_ABSTIME=1 — fall back to relative sleep */
    (void)clockid; /* Ignore clock ID, just sleep */
    return sys_nanosleep(g, req_gva, rem_gva);
}

/* ---------- Identity syscalls ---------- */

static int64_t sys_gettimeofday(guest_t *g, uint64_t tv_gva, uint64_t tz_gva) {
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

static int64_t sys_ftruncate(int fd, int64_t length) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (ftruncate(host_fd, length) < 0)
        return linux_errno();
    return 0;
}

/* Translate macOS struct statfs to Linux struct statfs */
static void translate_statfs(const struct statfs *mac, linux_statfs_t *lin) {
    memset(lin, 0, sizeof(*lin));
    lin->f_type    = mac->f_type;
    lin->f_bsize   = mac->f_bsize;
    lin->f_blocks  = mac->f_blocks;
    lin->f_bfree   = mac->f_bfree;
    lin->f_bavail  = mac->f_bavail;
    lin->f_files   = mac->f_files;
    lin->f_ffree   = mac->f_ffree;
    /* f_fsid is a struct with two ints on both platforms */
    lin->f_fsid[0] = mac->f_fsid.val[0];
    lin->f_fsid[1] = mac->f_fsid.val[1];
    lin->f_namelen = 255;  /* macOS doesn't expose this; 255 is typical */
    lin->f_frsize  = mac->f_bsize;  /* macOS has no frsize, use bsize */
}

static int64_t sys_statfs(guest_t *g, uint64_t path_gva, uint64_t buf_gva) {
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    struct statfs mac_st;
    if (statfs(path, &mac_st) < 0)
        return linux_errno();

    linux_statfs_t lin_st;
    translate_statfs(&mac_st, &lin_st);

    if (guest_write(g, buf_gva, &lin_st, sizeof(lin_st)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

static int64_t sys_fstatfs(guest_t *g, int fd, uint64_t buf_gva) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    struct statfs mac_st;
    if (fstatfs(host_fd, &mac_st) < 0)
        return linux_errno();

    linux_statfs_t lin_st;
    translate_statfs(&mac_st, &lin_st);

    if (guest_write(g, buf_gva, &lin_st, sizeof(lin_st)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

/* ---------- Batch 1: File manipulation ---------- */

static int64_t sys_mknodat(guest_t *g, int dirfd, uint64_t path_gva,
                           int mode, int dev) {
    (void)dev;
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    int host_dirfd = resolve_dirfd(dirfd);
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD) return -LINUX_EBADF;

    /* Only support FIFO creation; other node types need root */
    if (S_ISFIFO(mode)) {
        if (mkfifo(path, mode & 0777) < 0)
            return linux_errno();
        return 0;
    }

    /* Regular files: create an empty file */
    if (S_ISREG(mode) || (mode & S_IFMT) == 0) {
        int fd = openat(host_dirfd, path, O_CREAT | O_WRONLY | O_EXCL, mode & 0777);
        if (fd < 0) return linux_errno();
        close(fd);
        return 0;
    }

    return -LINUX_ENOSYS;
}

static int64_t sys_symlinkat(guest_t *g, uint64_t target_gva,
                             int dirfd, uint64_t linkpath_gva) {
    char target[4096], linkpath[4096];
    if (guest_read_str(g, target_gva, target, sizeof(target)) < 0)
        return -LINUX_EFAULT;
    if (guest_read_str(g, linkpath_gva, linkpath, sizeof(linkpath)) < 0)
        return -LINUX_EFAULT;

    int host_dirfd = resolve_dirfd(dirfd);
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD) return -LINUX_EBADF;

    if (symlinkat(target, host_dirfd, linkpath) < 0)
        return linux_errno();

    return 0;
}

static int64_t sys_linkat(guest_t *g, int olddirfd, uint64_t oldpath_gva,
                          int newdirfd, uint64_t newpath_gva, int flags) {
    char oldpath[4096], newpath[4096];
    if (guest_read_str(g, oldpath_gva, oldpath, sizeof(oldpath)) < 0)
        return -LINUX_EFAULT;
    if (guest_read_str(g, newpath_gva, newpath, sizeof(newpath)) < 0)
        return -LINUX_EFAULT;

    int host_olddir = resolve_dirfd(olddirfd);
    int host_newdir = resolve_dirfd(newdirfd);
    if (host_olddir < 0 && olddirfd != LINUX_AT_FDCWD) return -LINUX_EBADF;
    if (host_newdir < 0 && newdirfd != LINUX_AT_FDCWD) return -LINUX_EBADF;

    int mac_flags = translate_at_flags(flags);
    if (linkat(host_olddir, oldpath, host_newdir, newpath, mac_flags) < 0)
        return linux_errno();

    return 0;
}

static int64_t sys_fchmod(int fd, uint32_t mode) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (fchmod(host_fd, mode) < 0)
        return linux_errno();
    return 0;
}

static int64_t sys_fchmodat(guest_t *g, int dirfd, uint64_t path_gva,
                            uint32_t mode, int flags) {
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    int host_dirfd = resolve_dirfd(dirfd);
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD) return -LINUX_EBADF;

    /* macOS fchmodat doesn't support AT_SYMLINK_NOFOLLOW */
    int mac_flags = 0;
    if (flags & LINUX_AT_SYMLINK_NOFOLLOW) {
        /* Best effort: use lstat to check if it's a symlink */
        struct stat st;
        if (fstatat(host_dirfd, path, &st, AT_SYMLINK_NOFOLLOW) == 0
            && S_ISLNK(st.st_mode)) {
            return -LINUX_EOPNOTSUPP;
        }
    }

    if (fchmodat(host_dirfd, path, mode, mac_flags) < 0)
        return linux_errno();

    return 0;
}

static int64_t sys_fchownat(guest_t *g, int dirfd, uint64_t path_gva,
                            uint32_t owner, uint32_t group, int flags) {
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    int host_dirfd = resolve_dirfd(dirfd);
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD) return -LINUX_EBADF;

    int mac_flags = translate_at_flags(flags);
    if (fchownat(host_dirfd, path, owner, group, mac_flags) < 0)
        return linux_errno();

    return 0;
}

static int64_t sys_fchown(int fd, uint32_t owner, uint32_t group) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (fchown(host_fd, owner, group) < 0)
        return linux_errno();
    return 0;
}

static int64_t sys_utimensat(guest_t *g, int dirfd, uint64_t path_gva,
                             uint64_t times_gva, int flags) {
    int host_dirfd = resolve_dirfd(dirfd);
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD) return -LINUX_EBADF;

    /* If path is NULL (path_gva == 0), operate on the dirfd itself */
    const char *path_arg = NULL;
    char path[4096];
    if (path_gva != 0) {
        if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
            return -LINUX_EFAULT;
        path_arg = path;
    }

    struct timespec ts[2];
    if (times_gva != 0) {
        /* Read two linux_timespec_t from guest */
        linux_timespec_t lts[2];
        if (guest_read(g, times_gva, lts, sizeof(lts)) < 0)
            return -LINUX_EFAULT;

        /* UTIME_NOW = 0x3FFFFFFF, UTIME_OMIT = 0x3FFFFFFE (same on macOS) */
        ts[0].tv_sec = lts[0].tv_sec;
        ts[0].tv_nsec = lts[0].tv_nsec;
        ts[1].tv_sec = lts[1].tv_sec;
        ts[1].tv_nsec = lts[1].tv_nsec;
    }

    int mac_flags = 0;
    if (flags & LINUX_AT_SYMLINK_NOFOLLOW)
        mac_flags |= AT_SYMLINK_NOFOLLOW;

    if (utimensat(host_dirfd, path_arg, times_gva ? ts : NULL, mac_flags) < 0)
        return linux_errno();

    return 0;
}

/* ---------- Batch 2: Process/system info ---------- */

static int64_t sys_sched_getaffinity(guest_t *g, int pid, uint64_t size,
                                     uint64_t mask_gva) {
    (void)pid;
    /* Single-vCPU model: return a 1-CPU affinity mask.
     * The mask is a bitmask where bit 0 = CPU 0. */
    if (size < 8) return -LINUX_EINVAL;

    uint64_t mask = 1;  /* CPU 0 only */
    if (guest_write(g, mask_gva, &mask, 8) < 0)
        return -LINUX_EFAULT;

    return 8;  /* Returns size of written mask */
}

static int64_t sys_getgroups(guest_t *g, int size, uint64_t list_gva) {
    gid_t groups[64];
    int ngroups = getgroups(64, groups);
    if (ngroups < 0) return linux_errno();

    if (size == 0) return ngroups;
    if (size < ngroups) return -LINUX_EINVAL;

    /* Linux uses uint32_t for gid_t on aarch64 */
    uint32_t linux_groups[64];
    for (int i = 0; i < ngroups; i++)
        linux_groups[i] = (uint32_t)groups[i];

    if (guest_write(g, list_gva, linux_groups, ngroups * 4) < 0)
        return -LINUX_EFAULT;

    return ngroups;
}

static int64_t sys_getrusage(guest_t *g, int who, uint64_t usage_gva) {
    struct rusage mac_usage;
    if (getrusage(who, &mac_usage) < 0)
        return linux_errno();

    linux_rusage_t lin_usage;
    memset(&lin_usage, 0, sizeof(lin_usage));
    lin_usage.ru_utime.tv_sec  = mac_usage.ru_utime.tv_sec;
    lin_usage.ru_utime.tv_usec = mac_usage.ru_utime.tv_usec;
    lin_usage.ru_stime.tv_sec  = mac_usage.ru_stime.tv_sec;
    lin_usage.ru_stime.tv_usec = mac_usage.ru_stime.tv_usec;
    lin_usage.ru_maxrss  = mac_usage.ru_maxrss;
    lin_usage.ru_minflt  = mac_usage.ru_minflt;
    lin_usage.ru_majflt  = mac_usage.ru_majflt;
    lin_usage.ru_inblock = mac_usage.ru_inblock;
    lin_usage.ru_oublock = mac_usage.ru_oublock;
    lin_usage.ru_nvcsw   = mac_usage.ru_nvcsw;
    lin_usage.ru_nivcsw  = mac_usage.ru_nivcsw;

    if (guest_write(g, usage_gva, &lin_usage, sizeof(lin_usage)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

static int64_t sys_sysinfo(guest_t *g, uint64_t info_gva) {
    linux_sysinfo_t si;
    memset(&si, 0, sizeof(si));

    /* Uptime from boot time */
    struct timeval boottime;
    size_t bt_len = sizeof(boottime);
    int mib_bt[2] = { CTL_KERN, KERN_BOOTTIME };
    if (sysctl(mib_bt, 2, &boottime, &bt_len, NULL, 0) == 0) {
        struct timeval now;
        gettimeofday(&now, NULL);
        si.uptime = now.tv_sec - boottime.tv_sec;
    }

    /* Total RAM from hw.memsize */
    uint64_t memsize = 0;
    size_t ms_len = sizeof(memsize);
    int mib_mem[2] = { CTL_HW, HW_MEMSIZE };
    if (sysctl(mib_mem, 2, &memsize, &ms_len, NULL, 0) == 0) {
        si.totalram = memsize;
    }

    /* Free RAM from vm_statistics64 */
    vm_statistics64_data_t vmstat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vmstat, &count) == KERN_SUCCESS) {
        uint64_t page_size = 4096;
        si.freeram = (uint64_t)vmstat.free_count * page_size;
    }

    /* Load averages (× 65536 for fixed-point) */
    double loadavg[3];
    if (getloadavg(loadavg, 3) == 3) {
        si.loads[0] = (uint64_t)(loadavg[0] * 65536.0);
        si.loads[1] = (uint64_t)(loadavg[1] * 65536.0);
        si.loads[2] = (uint64_t)(loadavg[2] * 65536.0);
    }

    si.mem_unit = 1;
    si.procs = 1;  /* Single-process model */

    if (guest_write(g, info_gva, &si, sizeof(si)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

/* Translate Linux RLIMIT_* resource numbers to macOS equivalents.
 * The numbering differs: Linux RLIMIT_NPROC=6 vs macOS RLIMIT_NPROC=7. */
static int translate_rlimit_resource(int linux_res) {
    switch (linux_res) {
    case 0:  return RLIMIT_CPU;
    case 1:  return RLIMIT_FSIZE;
    case 2:  return RLIMIT_DATA;
    case 3:  return RLIMIT_STACK;
    case 4:  return RLIMIT_CORE;
    case 5:  return RLIMIT_RSS;
    case 6:  return RLIMIT_NPROC;      /* Linux 6 → macOS 7 */
    case 7:  return RLIMIT_NOFILE;     /* Linux 7 → macOS 8 */
    case 8:  return RLIMIT_MEMLOCK;    /* Linux 8 → macOS 6 */
    case 9:  return RLIMIT_AS;
    default: return -1;
    }
}

static int64_t sys_prlimit64(guest_t *g, int pid, int resource,
                             uint64_t new_gva, uint64_t old_gva) {
    (void)pid;  /* Ignore PID; single-process model */

    int mac_res = translate_rlimit_resource(resource);
    if (mac_res < 0) return -LINUX_EINVAL;

    /* Set new limits if requested */
    if (new_gva != 0) {
        linux_rlimit64_t new_lim;
        if (guest_read(g, new_gva, &new_lim, sizeof(new_lim)) < 0)
            return -LINUX_EFAULT;

        struct rlimit rl;
        rl.rlim_cur = new_lim.rlim_cur;
        rl.rlim_max = new_lim.rlim_max;
        if (setrlimit(mac_res, &rl) < 0)
            return linux_errno();
    }

    /* Get current limits if requested */
    if (old_gva != 0) {
        struct rlimit rl;
        if (getrlimit(mac_res, &rl) < 0)
            return linux_errno();

        linux_rlimit64_t old_lim;
        old_lim.rlim_cur = rl.rlim_cur;
        old_lim.rlim_max = rl.rlim_max;
        if (guest_write(g, old_gva, &old_lim, sizeof(old_lim)) < 0)
            return -LINUX_EFAULT;
    }

    return 0;
}

/* ---------- Batch 3: I/O optimization + sync ---------- */

static int64_t sys_fallocate(int fd, int mode, int64_t offset, int64_t len) {
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

static int64_t sys_sendfile(guest_t *g, int out_fd, int in_fd,
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

static int64_t sys_copy_file_range(guest_t *g, int fd_in, uint64_t off_in_gva,
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

/* ---------- Batch 4: Signals + I/O multiplexing ---------- */

static int64_t sys_ppoll(guest_t *g, uint64_t fds_gva, uint32_t nfds,
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

static int64_t sys_pselect6(guest_t *g, int nfds, uint64_t readfds_gva,
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

/* ---------- Quick-win syscalls ---------- */

static int64_t sys_fchdir(int fd) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (fchdir(host_fd) < 0)
        return linux_errno();
    return 0;
}

static int64_t sys_truncate(guest_t *g, uint64_t path_gva, int64_t length) {
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;
    if (truncate(path, length) < 0)
        return linux_errno();
    return 0;
}

static int64_t sys_close_range(unsigned int first, unsigned int last,
                                unsigned int flags) {
    (void)flags;
    if (first > last || last >= (unsigned)FD_TABLE_SIZE) {
        if (last >= (unsigned)FD_TABLE_SIZE) last = FD_TABLE_SIZE - 1;
    }
    for (unsigned int i = first; i <= last && i < (unsigned)FD_TABLE_SIZE; i++) {
        if (fd_table[i].type != FD_CLOSED) {
            if (fd_table[i].dir) {
                closedir((DIR *)fd_table[i].dir);
                fd_table[i].dir = NULL;
            }
            if (fd_table[i].type != FD_STDIO)
                close(fd_table[i].host_fd);
            fd_table[i].type = FD_CLOSED;
            fd_table[i].host_fd = -1;
            fd_table[i].linux_flags = 0;
        }
    }
    return 0;
}

static int64_t sys_statx(guest_t *g, int dirfd, uint64_t path_gva,
                          int flags, unsigned int mask, uint64_t statxbuf_gva) {
    (void)mask;
    char path[4096];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    int host_dirfd = resolve_dirfd(dirfd);
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD) return -LINUX_EBADF;

    int mac_flags = translate_at_flags(flags);
    struct stat mac_st;
    if (fstatat(host_dirfd, path, &mac_st, mac_flags) < 0)
        return linux_errno();

    /* Translate struct stat → struct statx */
    linux_statx_t sx;
    memset(&sx, 0, sizeof(sx));
    sx.stx_mask = STATX_BASIC_STATS;
    sx.stx_blksize = (uint32_t)mac_st.st_blksize;
    sx.stx_nlink = (uint32_t)mac_st.st_nlink;
    sx.stx_uid = mac_st.st_uid;
    sx.stx_gid = mac_st.st_gid;
    sx.stx_mode = (uint16_t)mac_st.st_mode;
    sx.stx_ino = mac_st.st_ino;
    sx.stx_size = mac_st.st_size;
    sx.stx_blocks = mac_st.st_blocks;
    sx.stx_atime_sec = mac_st.st_atimespec.tv_sec;
    sx.stx_atime_nsec = (uint32_t)mac_st.st_atimespec.tv_nsec;
    sx.stx_mtime_sec = mac_st.st_mtimespec.tv_sec;
    sx.stx_mtime_nsec = (uint32_t)mac_st.st_mtimespec.tv_nsec;
    sx.stx_ctime_sec = mac_st.st_ctimespec.tv_sec;
    sx.stx_ctime_nsec = (uint32_t)mac_st.st_ctimespec.tv_nsec;
    sx.stx_btime_sec = mac_st.st_birthtimespec.tv_sec;
    sx.stx_btime_nsec = (uint32_t)mac_st.st_birthtimespec.tv_nsec;
    sx.stx_mask |= STATX_BTIME;
    sx.stx_rdev_major = (uint32_t)(mac_st.st_rdev >> 24);
    sx.stx_rdev_minor = (uint32_t)(mac_st.st_rdev & 0xFFFFFF);
    sx.stx_dev_major = (uint32_t)(mac_st.st_dev >> 24);
    sx.stx_dev_minor = (uint32_t)(mac_st.st_dev & 0xFFFFFF);

    if (guest_write(g, statxbuf_gva, &sx, sizeof(sx)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

/* ---------- splice/tee/vmsplice emulation ---------- */

/* splice: emulate by reading from in_fd and writing to out_fd */
static int64_t sys_splice(guest_t *g, int fd_in, uint64_t off_in_gva,
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
static int64_t sys_vmsplice(guest_t *g, int fd, uint64_t iov_gva,
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
static int64_t sys_tee(int fd_in, int fd_out, size_t len, unsigned int flags) {
    (void)fd_in; (void)fd_out; (void)len; (void)flags;
    return -LINUX_EINVAL;
}

/* ---------- setitimer/getitimer ---------- */

/* Linux struct itimerval (same layout as macOS on LP64) */
typedef struct {
    linux_timeval_t it_interval;
    linux_timeval_t it_value;
} linux_itimerval_t;

static int64_t sys_setitimer(guest_t *g, int which,
                              uint64_t new_gva, uint64_t old_gva) {
    linux_itimerval_t lnew;
    if (guest_read(g, new_gva, &lnew, sizeof(lnew)) < 0)
        return -LINUX_EFAULT;

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

static int64_t sys_getitimer(guest_t *g, int which, uint64_t val_gva) {
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
    case SYS_readv:
        result = sys_readv(g, (int)x0, x1, (int)x2);
        break;
    case SYS_writev:
        result = sys_writev(g, (int)x0, x1, (int)x2);
        break;
    case SYS_pread64:
        result = sys_pread64(g, (int)x0, x1, x2, (int64_t)x3);
        break;
    case SYS_pwrite64:
        result = sys_pwrite64(g, (int)x0, x1, x2, (int64_t)x3);
        break;
    case SYS_ioctl:
        result = sys_ioctl(g, (int)x0, x1, x2);
        break;
    case SYS_fstat:
        result = sys_fstat(g, (int)x0, x1);
        break;
    case SYS_newfstatat:
        result = sys_newfstatat(g, (int)x0, x1, x2, (int)x3);
        break;
    case SYS_set_tid_address:
        result = proc_get_pid();
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
        result = proc_get_pid();
        break;
    case SYS_gettid:
        result = proc_get_pid();
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

    /* ---- Tier 3: file system ops ---- */
    case SYS_mkdirat:
        result = sys_mkdirat(g, (int)x0, x1, (int)x2);
        break;
    case SYS_unlinkat:
        result = sys_unlinkat(g, (int)x0, x1, (int)x2);
        break;
    case SYS_renameat2:
        result = sys_renameat2(g, (int)x0, x1, (int)x2, x3, (int)x4);
        break;
    case SYS_nanosleep:
        result = sys_nanosleep(g, x0, x1);
        break;
    case SYS_clock_nanosleep:
        result = sys_clock_nanosleep(g, (int)x0, (int)x1, x2, x3);
        break;

    /* ---- Identity and info syscalls ---- */
    case SYS_getuid:
    case SYS_geteuid:
    case SYS_getgid:
    case SYS_getegid:
        result = 1000;
        break;
    case SYS_gettimeofday:
        result = sys_gettimeofday(g, x0, x1);
        break;
    case SYS_ftruncate:
        result = sys_ftruncate((int)x0, (int64_t)x1);
        break;
    case SYS_statfs:
        result = sys_statfs(g, x0, x1);
        break;
    case SYS_fstatfs:
        result = sys_fstatfs(g, (int)x0, x1);
        break;
    case SYS_umask:
        result = (int64_t)umask((mode_t)x0);
        break;
    case SYS_madvise:
        result = 0;  /* Stub: ignore memory advice */
        break;

    /* ---- Networking stubs (not supported) ---- */
    case SYS_socket:
    case SYS_bind:
    case SYS_listen:
    case SYS_connect:
    case SYS_accept:
        result = -LINUX_ENOSYS;
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
    case SYS_fchdir:
        result = sys_fchdir((int)x0);
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

    /* ---- Batch 1: File manipulation ---- */
    case SYS_mknodat:
        result = sys_mknodat(g, (int)x0, x1, (int)x2, (int)x3);
        break;
    case SYS_symlinkat:
        result = sys_symlinkat(g, x0, (int)x1, x2);
        break;
    case SYS_linkat:
        result = sys_linkat(g, (int)x0, x1, (int)x2, x3, (int)x4);
        break;
    case SYS_fchmod:
        result = sys_fchmod((int)x0, (uint32_t)x1);
        break;
    case SYS_fchmodat:
        result = sys_fchmodat(g, (int)x0, x1, (uint32_t)x2, (int)x3);
        break;
    case SYS_fchownat:
        result = sys_fchownat(g, (int)x0, x1, (uint32_t)x2, (uint32_t)x3, (int)x4);
        break;
    case SYS_fchown:
        result = sys_fchown((int)x0, (uint32_t)x1, (uint32_t)x2);
        break;
    case SYS_utimensat:
        result = sys_utimensat(g, (int)x0, x1, x2, (int)x3);
        break;
    case SYS_futex:
        /* Single-threaded stub: WAKE returns 0, WAIT returns -EAGAIN */
        if (((int)x1 & 0x7F) == LINUX_FUTEX_WAKE)
            result = 0;
        else
            result = -LINUX_EAGAIN;
        break;
    case SYS_set_robust_list:
        result = 0;  /* Stub: single-threaded, no robust futexes */
        break;
    case SYS_sigaltstack:
        result = 0;  /* Stub: no signal delivery */
        break;

    /* ---- Batch 2: Process/system info ---- */
    case SYS_sched_getaffinity:
        result = sys_sched_getaffinity(g, (int)x0, x1, x2);
        break;
    case SYS_getpgid:
        result = proc_get_pid();
        break;
    case SYS_getgroups:
        result = sys_getgroups(g, (int)x0, x1);
        break;
    case SYS_getrusage:
        result = sys_getrusage(g, (int)x0, x1);
        break;
    case SYS_prctl:
        /* PR_SET_NAME/PR_GET_NAME: stub for thread naming */
        if ((int)x0 == LINUX_PR_SET_NAME || (int)x0 == LINUX_PR_GET_NAME)
            result = 0;
        else
            result = -LINUX_ENOSYS;
        break;
    case SYS_getppid:
        result = proc_get_ppid();
        break;
    case SYS_sysinfo:
        result = sys_sysinfo(g, x0);
        break;
    case SYS_prlimit64:
        result = sys_prlimit64(g, (int)x0, (int)x1, x2, x3);
        break;

    /* ---- Batch 3: I/O optimization + sync ---- */
    case SYS_fallocate:
        result = sys_fallocate((int)x0, (int)x1, (int64_t)x2, (int64_t)x3);
        break;
    case SYS_sendfile:
        result = sys_sendfile(g, (int)x0, (int)x1, x2, x3);
        break;
    case SYS_sync:
        sync();
        result = 0;
        break;
    case SYS_fsync:
        result = (fsync(fd_to_host((int)x0)) < 0) ? linux_errno() : 0;
        break;
    case SYS_fdatasync:
        /* macOS has no fdatasync; fsync is the closest equivalent */
        result = (fsync(fd_to_host((int)x0)) < 0) ? linux_errno() : 0;
        break;
    case SYS_sched_yield:
        sched_yield();
        result = 0;
        break;
    case SYS_copy_file_range:
        result = sys_copy_file_range(g, (int)x0, x1, (int)x2, x3, x4, (unsigned int)x5);
        break;

    /* ---- Batch 4: Signals + I/O multiplexing ---- */
    case SYS_ppoll:
        result = sys_ppoll(g, x0, (uint32_t)x1, x2, x3);
        break;
    case SYS_pselect6:
        result = sys_pselect6(g, (int)x0, x1, x2, x3, x4);
        break;
    case SYS_kill: {
        int64_t our_pid = proc_get_pid();
        if ((int)x0 == our_pid && (int)x1 == 0) result = 0;
        else if ((int)x0 == our_pid) result = 0;
        else result = -LINUX_ESRCH;
        break;
    }
    case SYS_tgkill: {
        int64_t our_pid = proc_get_pid();
        if ((int)x1 == our_pid) result = 0;
        else result = -LINUX_ESRCH;
        break;
    }
    case SYS_rt_sigsuspend:
        result = -LINUX_EINTR;  /* Return immediately (no signals) */
        break;
    case SYS_rt_sigpending:
        /* Return empty signal set */
        if (x0 != 0) {
            uint64_t empty = 0;
            guest_write(g, x0, &empty, 8);
        }
        result = 0;
        break;
    case SYS_rt_sigreturn:
        result = -LINUX_ENOSYS;  /* Should never be called */
        break;
    case SYS_setpgid:
        result = 0;  /* Stub: process groups not meaningful */
        break;
    case SYS_setsid:
        result = proc_get_pid();
        break;

    /* ---- Quick-win syscalls ---- */
    case SYS_truncate:
        result = sys_truncate(g, x0, (int64_t)x1);
        break;
    case SYS_flock:
        /* flock is a no-op stub (advisory locking not critical) */
        result = 0;
        break;
    case SYS_setuid:
    case SYS_setgid:
    case SYS_setreuid:
    case SYS_setregid:
    case SYS_setresuid:
    case SYS_setresgid:
        result = 0;  /* Stub: pretend success */
        break;
    case SYS_getresuid: {
        /* Write {1000,1000,1000} to guest pointers */
        uint32_t uid = 1000;
        if (x0) guest_write(g, x0, &uid, 4);
        if (x1) guest_write(g, x1, &uid, 4);
        if (x2) guest_write(g, x2, &uid, 4);
        result = 0;
        break;
    }
    case SYS_getresgid: {
        uint32_t gid = 1000;
        if (x0) guest_write(g, x0, &gid, 4);
        if (x1) guest_write(g, x1, &gid, 4);
        if (x2) guest_write(g, x2, &gid, 4);
        result = 0;
        break;
    }
    case SYS_setpriority:
        result = 0;  /* Stub: pretend success */
        break;
    case SYS_getpriority:
        result = 20;  /* Default nice value */
        break;
    case SYS_close_range:
        result = sys_close_range((unsigned int)x0, (unsigned int)x1,
                                  (unsigned int)x2);
        break;
    case SYS_statx:
        result = sys_statx(g, (int)x0, x1, (int)x2, (unsigned int)x3, x4);
        break;

    /* ---- Process management ---- */
    case SYS_execve:
        result = sys_execve(vcpu, g, x0, x1, x2, verbose);
        if (result == SYSCALL_EXEC_HAPPENED)
            return SYSCALL_EXEC_HAPPENED;
        break;
    case SYS_execveat:
        /* execveat with AT_EMPTY_PATH + fd is complex; stub with execve
         * semantics when dirfd is AT_FDCWD */
        if ((int)x0 == LINUX_AT_FDCWD) {
            result = sys_execve(vcpu, g, x1, x2, x3, verbose);
            if (result == SYSCALL_EXEC_HAPPENED)
                return SYSCALL_EXEC_HAPPENED;
        } else {
            result = -LINUX_ENOSYS;
        }
        break;
    case SYS_clone:
        result = sys_clone(vcpu, g, x0, x1, x2, x3, x4, verbose);
        break;
    case SYS_wait4:
        result = sys_wait4(g, (int)x0, x1, (int)x2, x3);
        break;
    case SYS_waitid:
        /* Minimal stub: translate to wait4 semantics for common cases */
        result = -LINUX_ENOSYS;
        break;

    /* ---- Splice/tee/vmsplice emulation ---- */
    case SYS_splice:
        result = sys_splice(g, (int)x0, x1, (int)x2, x3, (size_t)x4,
                             (unsigned int)x5);
        break;
    case SYS_vmsplice:
        result = sys_vmsplice(g, (int)x0, x1, (unsigned long)x2,
                               (unsigned int)x3);
        break;
    case SYS_tee:
        result = sys_tee((int)x0, (int)x1, (size_t)x2, (unsigned int)x3);
        break;

    /* ---- Timer syscalls ---- */
    case SYS_setitimer:
        result = sys_setitimer(g, (int)x0, x1, x2);
        break;
    case SYS_getitimer:
        result = sys_getitimer(g, (int)x0, x1);
        break;
    case SYS_timerfd_create:
    case SYS_timerfd_settime:
    case SYS_timerfd_gettime:
        /* timerfd requires kqueue-based emulation — stub for now */
        result = -LINUX_ENOSYS;
        break;

    /* ---- epoll stubs (would need kqueue translation) ---- */
    case SYS_epoll_create1:
    case SYS_epoll_ctl:
    case SYS_epoll_pwait:
        result = -LINUX_ENOSYS;
        break;

    /* ---- inotify stubs ---- */
    case SYS_inotify_init1:
    case SYS_inotify_add_watch:
    case SYS_inotify_rm_watch:
        result = -LINUX_ENOSYS;
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

        /* Signal the shim to flush TLB if page tables were modified.
         * X8 is the syscall number register — Linux ABI marks it as
         * clobbered, so reusing it for TLBI signalling is safe. */
        if (g->need_tlbi) {
            hv_vcpu_set_reg(vcpu, HV_REG_X8, 1);
            g->need_tlbi = 0;
        } else {
            hv_vcpu_set_reg(vcpu, HV_REG_X8, 0);
        }
    }

    return should_exit;
}

/* Stack builder moved to stack.c/stack.h */
