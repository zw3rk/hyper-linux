/* syscall_fs.c — Filesystem syscall handlers for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stat, open, close, directory, xattr, permissions, and other filesystem
 * operations. All functions are called from syscall_dispatch() in syscall.c.
 */
#include "syscall_fs.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "syscall_fd.h"
#include "syscall_inotify.h"
#include "proc_emulation.h"
#include "syscall_proc.h"   /* proc_get_sysroot */
#include "guest.h"
#include "fd_object.h"
#include "syscall_stats.h"   /* syscall_stats_clear_fd */
#include "vfs.h"
#include "device.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/xattr.h>
#include <pthread.h>

/* ---------- stat translation ---------- */

/* Re-encode macOS dev_t to Linux new_encode_dev format.
 * macOS: major = (dev >> 24) & 0xFF, minor = dev & 0xFFFFFF
 * Linux: (minor & 0xFF) | (major << 8) | ((minor & ~0xFF) << 12) */
static uint64_t mac_to_linux_dev(dev_t dev) {
    unsigned int maj = ((unsigned int)dev >> 24) & 0xFF;
    unsigned int min = (unsigned int)dev & 0xFFFFFF;
    return (uint64_t)((min & 0xFF) | (maj << 8) | ((min & ~0xFFU) << 12));
}

/* Translate macOS struct stat to Linux aarch64 struct stat */
static void translate_stat(const struct stat *mac, linux_stat_t *lin) {
    memset(lin, 0, sizeof(*lin));
    lin->st_dev     = mac_to_linux_dev(mac->st_dev);
    lin->st_ino     = mac->st_ino;
    lin->st_mode    = mac->st_mode;
    lin->st_nlink   = (uint32_t)mac->st_nlink;
    lin->st_uid     = mac->st_uid;
    lin->st_gid     = mac->st_gid;
    lin->st_rdev    = mac_to_linux_dev(mac->st_rdev);
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

/* ---------- Linux dirent64 layout ---------- */
typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    /* char d_name[] follows */
} __attribute__((packed)) linux_dirent64_t;

/* ---------- sysroot path resolution ---------- */

/* Resolve an absolute path through the sysroot: tries sysroot+path first,
 * then sysroot/lib/basename as fallback for nix store library paths.
 * Returns the resolved path (may point into buf) or the original path
 * if no sysroot is set or no match is found. */
static const char *resolve_sysroot_path(const char *path, char *buf, size_t bufsz) {
    const char *sr = proc_get_sysroot();
    if (!sr || !sr[0])
        sr = hl_vfs_sysroot();
    if (!sr || !sr[0] || !path || path[0] != '/') return path;

    /* Never rewrite bare "/": sysroot+"/" is a directory and would match
     * every root open via existence check. */
    if (path[1] == '\0')
        return path;

    snprintf(buf, bufsz, "%s%s", sr, path);
    if (access(buf, F_OK) == 0) return buf;

    /* Fallback: sysroot/lib/basename — handles nix store library paths
     * (absolute RUNPATH entries outside rooted binds). */
    const char *base = strrchr(path, '/');
    if (base && base[1] && strchr(base + 1, '/') == NULL) {
        /* Only bare sonames / basenames, not multi-component leftovers. */
        snprintf(buf, bufsz, "%s/lib/%s", sr, base + 1);
        if (access(buf, F_OK) == 0) return buf;
    }
    return path;
}

/* Unified guest path → host path for ALL pathname syscalls.
 * Rooted mode: one mount/CWD resolver (hl_vfs_resolve_at).
 * Legacy mode: sysroot existence checks (prior behavior).
 *
 * On success: writes host path into out_buf, sets *out_path, returns 0.
 * On error: returns negative Linux errno.
 * create_mode=1 for O_CREAT/mkdir/rename-dest (final may not exist).
 * out_mount may be NULL; when set, receives mount used (for EXDEV checks).
 */
/* Synthetic mount for the --sysroot fallback, which resolves outside the
 * mount table. hl binds the sysroot read-only (hl.c), so report RO here or
 * every caller's `mnt && (mnt->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL))` guard is dead on this
 * path and writes reach the host through a mount declared read-only. */
static const hl_mount_t sysroot_fallback_mount = {
    .id = 0,
    .guest_prefix = "/",
    .host_path = "",
    .flags = HL_MOUNT_RO,
    .guest_len = 1,
};

/* follow == 0 means "operate on the final component itself, do not follow a
 * symlink there" — readlink/lstat/unlink/rename/symlink/O_NOFOLLOW. Most
 * callers want the default (follow == 1), supplied by the
 * resolve_path_for_op() wrapper macro defined after this function. */
static int resolve_path_for_op_ex(int dirfd, const char *guest_path,
                               int create_mode, int follow,
                               char *out_buf, size_t out_sz,
                               const char **out_path,
                               const hl_mount_t **out_mount) {
    if (out_mount) *out_mount = NULL;
    if (!guest_path || !out_buf || !out_path) return -LINUX_EFAULT;

    if (hl_vfs_mode() == HL_FS_ROOTED) {
        hl_vfs_resolve_t r;
        int rc = hl_vfs_resolve_at(dirfd, guest_path, follow, create_mode, &r);
        if (rc < 0) {
            /* Dynamic linker probes absolute nix RPATHs outside binds.
             * Fall back to --sysroot /lib/basename for library loads. */
            if (guest_path[0] == '/' && !create_mode) {
                char tmp[LINUX_PATH_MAX];
                const char *sr = resolve_sysroot_path(guest_path, tmp,
                                                      sizeof(tmp));
                if (sr != guest_path) {
                    snprintf(out_buf, out_sz, "%s", sr);
                    *out_path = out_buf;
                    /* This path bypassed the mount table, so callers would
                     * otherwise see mount == NULL and skip their RO check
                     * entirely. hl binds the sysroot read-only; report a
                     * synthetic RO mount so those guards still fire. */
                    if (out_mount) *out_mount = &sysroot_fallback_mount;
                    if (hl_trace_on(HL_TRACE_FS))
                        hl_trace(HL_TRACE_FS,
                                 "pathop sysroot-fallback guest=%s host=%s",
                                 guest_path, out_buf);
                    return 0;
                }
            }
            return rc;
        }
        if (r.is_virtual) {
            /* Virtual mounts have no host path for mutation ops */
            snprintf(out_buf, out_sz, "%s", r.guest_abs);
            *out_path = out_buf;
            if (out_mount) *out_mount = r.mount;
            return 0;
        }
        if (r.mount && (r.mount->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL)) && create_mode)
            return -LINUX_EROFS;
        snprintf(out_buf, out_sz, "%s", r.host_path);
        *out_path = out_buf;
        if (out_mount) *out_mount = r.mount;
        if (hl_trace_on(HL_TRACE_FS))
            hl_trace(HL_TRACE_FS, "pathop guest=%s host=%s create=%d",
                     r.guest_abs, r.host_path, create_mode);
        return 0;
    }

    /* Legacy */
    const char *p = resolve_sysroot_path(guest_path, out_buf, out_sz);
    if (p != out_buf)
        snprintf(out_buf, out_sz, "%s", p);
    *out_path = out_buf;
    return 0;
}

/* Most path ops follow a symlink in the final component. The ones that must
 * not (readlink, unlink, rename, symlink, lstat, O_NOFOLLOW) call
 * resolve_path_for_op_ex directly with follow = 0. */
#define resolve_path_for_op(dirfd, gp, cm, buf, sz, op, om) \
    resolve_path_for_op_ex((dirfd), (gp), (cm), 1, (buf), (sz), (op), (om))

/* After rooted resolve, use AT_FDCWD + absolute host path. */
static int host_dirfd_for_resolved(void) {
    return AT_FDCWD;
}

/* Which host dirfd to pass alongside a path from resolve_path_for_op_ex().
 *
 * Rooted mode always yields an absolute host path, so AT_FDCWD is correct.
 * Legacy mode does NOT: it only applies the --sysroot redirect to ABSOLUTE
 * paths and hands relative ones back verbatim. Pairing those with AT_FDCWD
 * reinterprets them against the process CWD instead of the caller's dirfd —
 * so renameat2/linkat/fchmodat/fchownat/utimensat silently operated on a
 * same-named file in the wrong directory and returned 0.
 * Returns AT_FDCWD (-2 on macOS) or a valid fd on success, and exactly -1
 * for a bad guest fd — so callers must test `== -1`, not `< 0`. */
static int host_dirfd_for_op(int dirfd) {
    if (hl_vfs_mode() == HL_FS_ROOTED || dirfd == LINUX_AT_FDCWD)
        return AT_FDCWD;
    int hfd = fd_to_host(dirfd);
    return hfd < 0 ? -1 : hfd;
}

/* Synthetic directory: the immediate children of a mount-ancestor path,
 * snapshotted at open() the way a real DIR* snapshots its stream. */
typedef struct {
    int  n;
    int  pos;
    char names[HL_VFS_SYNTH_MAX][HL_VFS_NAME_MAX];
} hl_vdir_t;

/* stat() for a synthetic directory: a read-only, execute-searchable dir
 * with no host inode behind it. */
static int64_t vdir_fill_stat(guest_t *g, uint64_t stat_gva) {
    struct stat mac_st;
    memset(&mac_st, 0, sizeof(mac_st));
    mac_st.st_mode  = S_IFDIR | 0555;
    mac_st.st_nlink = 2;
    mac_st.st_uid   = getuid();
    mac_st.st_gid   = getgid();
    mac_st.st_size  = 512;
    mac_st.st_ino   = 1;
    linux_stat_t lin_st;
    translate_stat(&mac_st, &lin_st);
    if (guest_write(g, stat_gva, &lin_st, sizeof(lin_st)) < 0)
        return -LINUX_EFAULT;
    return 0;
}

static hl_vdir_t *vdir_create(const char *guest_abs) {
    hl_vdir_t *vd = calloc(1, sizeof(*vd));
    if (!vd) return NULL;
    vd->n = hl_vfs_list_synthetic(guest_abs, vd->names, HL_VFS_SYNTH_MAX);
    return vd;
}

/* ---------- open/close ---------- */

int64_t sys_openat(guest_t *g, int dirfd, uint64_t path_gva,
                   int linux_flags, int mode) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    /* Virtual device registry (OSS /dev/dsp, migrated pseudo devices). */
    {
        int64_t dfd = hl_device_open(path, linux_flags, mode);
        if (dfd != -2)
            return dfd; /* success (>=0) or device error (<0, not -2) */
    }

    /* Intercept /proc and remaining /dev paths before host filesystem */
    int intercepted = proc_intercept_open(g, path, linux_flags, mode);
    if (intercepted >= 0) {
        /* Got a host fd from the intercept — allocate a guest fd for it.
         * Use fd range >= 128 to avoid collisions with GHC GC finalizers
         * that close stale low-numbered fds from previously-closed files. */
        int guest_fd = fd_alloc_from(128, FD_REGULAR, intercepted);
        if (guest_fd < 0) {
            close(intercepted);
            return -LINUX_ENOMEM;
        }
        fd_table[guest_fd].linux_flags = linux_flags;
        return guest_fd;
    }
    if (intercepted == -1) {
        /* Intercept matched but failed */
        return linux_errno();
    }
    /* intercepted == -2: not intercepted, fall through to real openat */

    int create_mode = (linux_flags & LINUX_O_CREAT) != 0;
    char open_path_buf[LINUX_PATH_MAX];
    char sysroot_buf[LINUX_PATH_MAX];
    const char *open_path = path;
    int host_dirfd;

    /* O_CREAT|O_EXCL never follows a final symlink on Linux: the point of
     * O_EXCL is that an existing name — symlink included — fails with
     * EEXIST. Following it meant the host open acted on the TARGET, so a
     * link to a name that does not exist yet made hl create the target
     * instead, wherever that pointed. The classic symlink attack. */
    int excl_create = (linux_flags & (LINUX_O_CREAT | LINUX_O_EXCL))
                      == (LINUX_O_CREAT | LINUX_O_EXCL);
    int follow_final = !(linux_flags & LINUX_O_NOFOLLOW) && !excl_create;

    if (hl_vfs_mode() == HL_FS_ROOTED) {
        hl_vfs_resolve_t r;
        int rc = hl_vfs_resolve_at(dirfd, path, follow_final, create_mode, &r);
        if (rc < 0) {
            /* Absolute nix-store / loader paths often fall outside binds.
             * Fall back to --sysroot basename resolution so dynamic
             * binaries (e.g. xmms) can open libgtk-1.2.so.0 etc. */
            if (path[0] == '/' && !create_mode) {
                const char *sr = resolve_sysroot_path(path, sysroot_buf,
                                                      sizeof(sysroot_buf));
                if (sr != path) {
                    open_path = sr;
                    host_dirfd = AT_FDCWD;
                    goto do_open;
                }
            }
            return rc;
        }
        if (r.is_virtual) {
            int64_t dfd = hl_device_open(r.guest_abs, linux_flags, mode);
            if (dfd != -2) return dfd;
            intercepted = proc_intercept_open(g, r.guest_abs, linux_flags, mode);
            if (intercepted >= 0) {
                int guest_fd = fd_alloc_from(128, FD_REGULAR, intercepted);
                if (guest_fd < 0) { close(intercepted); return -LINUX_ENOMEM; }
                fd_table[guest_fd].linux_flags = linux_flags;
                return guest_fd;
            }
            if (intercepted == -1) return linux_errno();

            /* A mount-ancestor directory — "/" itself, or an interior
             * component of a mount path. These exist only in the mount
             * table, so there is no host directory to open and every
             * opendir("/") failed with ENOENT. Hand back a synthetic
             * directory fd that getdents64 fills from the mount table. */
            if (hl_vfs_is_synthetic_dir(r.guest_abs)) {
                if (linux_flags & (LINUX_O_WRONLY | LINUX_O_RDWR |
                                   LINUX_O_CREAT))
                    return -LINUX_EISDIR;
                hl_vdir_t *vd = vdir_create(r.guest_abs);
                if (!vd) return -LINUX_ENOMEM;
                int guest_fd = fd_alloc(FD_VIRTUAL_DIR, -1);
                if (guest_fd < 0) { free(vd); return -LINUX_EMFILE; }
                fd_table[guest_fd].dir = vd;
                fd_table[guest_fd].linux_flags = linux_flags;
                return guest_fd;
            }
            return -LINUX_ENOENT;
        }
        if (r.mount && (r.mount->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL)) &&
            (linux_flags & (LINUX_O_WRONLY | LINUX_O_RDWR | LINUX_O_CREAT)))
            return -LINUX_EROFS;
        snprintf(open_path_buf, sizeof(open_path_buf), "%s", r.host_path);
        open_path = open_path_buf;
        host_dirfd = AT_FDCWD; /* resolved absolute host path */
        if (hl_trace_on(HL_TRACE_FS))
            hl_trace(HL_TRACE_FS, "openat guest=%s host=%s flags=0x%x",
                     r.guest_abs, open_path, linux_flags);
    } else {
        open_path = resolve_sysroot_path(path, sysroot_buf, sizeof(sysroot_buf));
        if (dirfd == LINUX_AT_FDCWD) {
            host_dirfd = AT_FDCWD;
        } else {
            host_dirfd = fd_to_host(dirfd);
            if (host_dirfd < 0) return -LINUX_EBADF;
        }
    }

do_open:
    ; /* C requires a statement after a label before declarations */
    {
    int flags = translate_open_flags(linux_flags);
    int host_fd = openat(host_dirfd, open_path, flags, mode);
    /* Rooted miss that looked like a host path: try sysroot basename. */
    if (host_fd < 0 && path[0] == '/' && !create_mode) {
        const char *sr = resolve_sysroot_path(path, sysroot_buf,
                                              sizeof(sysroot_buf));
        if (sr != path) {
            if (hl_trace_on(HL_TRACE_FS))
                hl_trace(HL_TRACE_FS, "openat sysroot-retry path=%s → %s",
                         path, sr);
            host_fd = openat(AT_FDCWD, sr, flags, mode);
        }
    }
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
    } /* do_open block */
}

/* Special-FD subsystems key their state on the GUEST fd number, so any path
 * that retires a guest fd must tell them — not just close(). dup3() onto a
 * live eventfd/signalfd/timerfd/inotify used to skip this, leaving the old
 * state alive under a number the fd table had handed to something else: the
 * next eventfd() to reuse that number inherited a stale entry and never
 * became poll-ready, hanging the reader. */
void fd_special_subsystem_close(int guest_fd, int type) {
    switch (type) {
    case FD_EVENTFD:  eventfd_close(guest_fd);  break;
    case FD_SIGNALFD: signalfd_close(guest_fd); break;
    case FD_TIMERFD:  timerfd_close(guest_fd);  break;
    case FD_INOTIFY:  inotify_close(guest_fd);  break;
    default: break;
    }
}

int64_t sys_close(int fd) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -LINUX_EBADF;

    /* Atomically snapshot and mark closed under fd_lock.  This prevents
     * a TOCTOU race where two concurrent sys_close() calls both read
     * the same open entry and double-close the host fd. */
    fd_entry_t snap;
    if (!fd_snapshot_and_close(fd, &snap))
        return -LINUX_EBADF;

    syscall_stats_clear_fd(fd);
    fd_special_subsystem_close(fd, snap.type);

    if (snap.desc) {
        /* Descriptor release closes host alias, dir, and open-file. */
        hl_descriptor_release(snap.desc);
        return 0;
    }

    /* Legacy path without descriptor object */
    if (snap.dir) {
        if (snap.type == FD_DIR)
            closedir((DIR *)snap.dir);
        else if (snap.type == FD_EPOLL || snap.type == FD_VIRTUAL_DIR)
            free(snap.dir);  /* epoll_instance_t / hl_vdir_t */
    }
    if (snap.of)
        hl_open_file_release(snap.of);

    /* Don't actually close stdin/stdout/stderr on the host */
    if (snap.type != FD_STDIO && snap.host_fd >= 0) {
        close(snap.host_fd);
    }
    return 0;
}

/* ---------- stat family ---------- */

int64_t sys_fstat(guest_t *g, int fd, uint64_t stat_gva) {
    {
        hl_fd_ref_t ref;
        if (hl_fd_get(fd, &ref) == 0) {
            if (ref.of && ref.of->ops && ref.of->ops->fstat) {
                int64_t r = ref.of->ops->fstat(ref.of, ref.host_fd, g, stat_gva);
                hl_fd_put(&ref);
                return r;
            }
            hl_fd_put(&ref);
        }
    }

    /* Synthetic directories have no host fd; report a plain read-only
     * directory so opendir()/fstatat(AT_EMPTY_PATH) work on them. */
    if (fd >= 0 && fd < FD_TABLE_SIZE && fd_table[fd].type == FD_VIRTUAL_DIR)
        return vdir_fill_stat(g, stat_gva);

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) {
        if (g->verbose) fprintf(stderr, "hl: fstat(%d): EBADF (no host fd)\n", fd);
        return -LINUX_EBADF;
    }

    struct stat mac_st;
    if (fstat(host_fd, &mac_st) < 0) {
        if (g->verbose) fprintf(stderr, "hl: fstat(%d→%d): host fstat failed errno=%d\n",
                                fd, host_fd, errno);
        return linux_errno();
    }

    /* A virtual /dev node is backed by the corresponding HOST device, so a
     * bare fstat reported the macOS major/minor while stat() and statx() on
     * the same path reported the Linux ones from the registry — /dev/null
     * came back 1:3 by path and 3:2 by fd. Take the declared values. */
    {
        uint32_t dmode = 0;
        uint64_t drdev = 0;
        if (hl_device_fd_stat(fd, &dmode, &drdev) == 0) {
            mac_st.st_mode = dmode;          /* registry mode is complete */
            mac_st.st_rdev = (dev_t)drdev;
        }
    }

    linux_stat_t lin_st;
    translate_stat(&mac_st, &lin_st);

    if (guest_write(g, stat_gva, &lin_st, sizeof(lin_st)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

int64_t sys_newfstatat(guest_t *g, int dirfd, uint64_t path_gva,
                       uint64_t stat_gva, int flags) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    int host_dirfd;
    if (dirfd == LINUX_AT_FDCWD) {
        host_dirfd = AT_FDCWD;
    } else {
        host_dirfd = fd_to_host(dirfd);
        if (host_dirfd < 0) return -LINUX_EBADF;
    }

    struct stat mac_st;

    /* AT_EMPTY_PATH with empty path: stat the fd itself (fstat).
     * macOS fstatat() doesn't support AT_EMPTY_PATH. */
    if ((flags & LINUX_AT_EMPTY_PATH) && path[0] == '\0') {
        if (host_dirfd < 0 || host_dirfd == AT_FDCWD) return -LINUX_EBADF;
        if (fstat(host_dirfd, &mac_st) < 0)
            return linux_errno();
    } else {
        /* Device/virtual synthetic first */
        uint32_t mode = 0;
        uint64_t rdev = 0;
        int ds = hl_device_stat(path, &mode, &rdev);
        if (ds == 0) {
            memset(&mac_st, 0, sizeof(mac_st));
            mac_st.st_mode = mode;
            mac_st.st_rdev = (dev_t)rdev;
            mac_st.st_nlink = 1;
        } else {
            /* Try /proc interception first (macOS has no /proc filesystem) */
            int intercepted = proc_intercept_stat(path, &mac_st);
            if (intercepted == -1) return linux_errno();
            if (intercepted == -2) {
                char hostbuf[LINUX_PATH_MAX];
                const char *stat_path = NULL;
                int rr = resolve_path_for_op_ex(dirfd, path, 0, !(flags & LINUX_AT_SYMLINK_NOFOLLOW), hostbuf,
                                             sizeof(hostbuf), &stat_path, NULL);
                if (rr < 0) return rr;
                int use_dirfd = (hl_vfs_mode() == HL_FS_ROOTED)
                    ? host_dirfd_for_resolved() : host_dirfd;
                int mac_flags = translate_at_flags(flags);
                if (fstatat(use_dirfd, stat_path, &mac_st, mac_flags) < 0)
                    return linux_errno();
            }
        }
    }

    linux_stat_t lin_st;
    translate_stat(&mac_st, &lin_st);

    if (guest_write(g, stat_gva, &lin_st, sizeof(lin_st)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

int64_t sys_statfs(guest_t *g, uint64_t path_gva, uint64_t buf_gva) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *fs_path = NULL;
    int rr = resolve_path_for_op(LINUX_AT_FDCWD, path, 0, hostbuf,
                                 sizeof(hostbuf), &fs_path, NULL);
    if (rr < 0) return rr;
    struct statfs mac_st;
    if (statfs(fs_path, &mac_st) < 0)
        return linux_errno();

    linux_statfs_t lin_st;
    translate_statfs(&mac_st, &lin_st);

    if (guest_write(g, buf_gva, &lin_st, sizeof(lin_st)) < 0)
        return -LINUX_EFAULT;

    return 0;
}

int64_t sys_fstatfs(guest_t *g, int fd, uint64_t buf_gva) {
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

int64_t sys_statx(guest_t *g, int dirfd, uint64_t path_gva,
                  int flags, unsigned int mask, uint64_t statxbuf_gva) {
    (void)mask;
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    int host_dirfd = resolve_dirfd(dirfd);
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD
        && hl_vfs_mode() != HL_FS_ROOTED)
        return -LINUX_EBADF;

    struct stat mac_st;

    /* AT_EMPTY_PATH with empty path: stat the fd itself (fstat).
     * macOS fstatat() doesn't support AT_EMPTY_PATH. */
    if ((flags & LINUX_AT_EMPTY_PATH) && path[0] == '\0') {
        /* Synthetic directories have no host fd at all. */
        if (dirfd >= 0 && dirfd < FD_TABLE_SIZE &&
            fd_table[dirfd].type == FD_VIRTUAL_DIR) {
            memset(&mac_st, 0, sizeof(mac_st));
            mac_st.st_mode = S_IFDIR | 0555;
            mac_st.st_nlink = 2;
            mac_st.st_size = 512;
            mac_st.st_ino = 1;
            goto statx_have_stat;
        }
        if (host_dirfd < 0 || host_dirfd == AT_FDCWD) return -LINUX_EBADF;
        if (fstat(host_dirfd, &mac_st) < 0)
            return linux_errno();
        /* ...and a device fd must report its registered numbers, exactly
         * as the path-based branch below does. */
        uint32_t emode = 0;
        uint64_t erdev = 0;
        if (hl_device_fd_stat(dirfd, &emode, &erdev) == 0) {
            mac_st.st_mode = emode;
            mac_st.st_rdev = (dev_t)erdev;
        }
    } else {
        /* Same synthetic /dev and /proc interception sys_newfstatat does.
         * glibc >= 2.33 implements stat()/lstat()/fstatat() on top of
         * statx, so without this stat("/proc/self/exe") and
         * stat("/dev/dsp") failed on a glibc guest while open() of the
         * very same path succeeded. */
        uint32_t dmode = 0;
        uint64_t drdev = 0;
        if (hl_device_stat(path, &dmode, &drdev) == 0) {
            memset(&mac_st, 0, sizeof(mac_st));
            mac_st.st_mode = dmode;
            mac_st.st_rdev = (dev_t)drdev;
            mac_st.st_nlink = 1;
            goto statx_have_stat;
        }
        {
            int pi = proc_intercept_stat(path, &mac_st);
            if (pi == -1) return linux_errno();
            if (pi == 0) goto statx_have_stat;
        }
        char hostbuf[LINUX_PATH_MAX];
        const char *statx_path = NULL;
        int rr = resolve_path_for_op_ex(dirfd, path, 0, !(flags & LINUX_AT_SYMLINK_NOFOLLOW), hostbuf, sizeof(hostbuf),
                                     &statx_path, NULL);
        if (rr < 0) return rr;
        if (hl_vfs_mode() == HL_FS_ROOTED)
            host_dirfd = host_dirfd_for_resolved();
        int mac_flags = translate_at_flags(flags);
        if (fstatat(host_dirfd, statx_path, &mac_st, mac_flags) < 0)
            return linux_errno();
    }

statx_have_stat:
    ;   /* C11: a label must be followed by a statement, not a declaration */
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

/* ---------- dup/fcntl ---------- */

int64_t sys_dup(int oldfd) {
    if (oldfd < 0 || oldfd >= FD_TABLE_SIZE) return -LINUX_EBADF;
    if (fd_table[oldfd].type == FD_CLOSED) return -LINUX_EBADF;
    /* Descriptor-backed fds must share the open-file description (POSIX).
     * fd_alloc() below explicitly NULLs .of/.desc, so going through the
     * legacy path gave the duplicate an independent object: closing the
     * original tore down the shared state (for /dev/dsp, the audio stream)
     * and broke the survivor. It also handles host_fd == -1 (e.g.
     * /dev/mixer), which the fd_to_host() check below rejects outright. */
    if (fd_table[oldfd].desc)
        return hl_fd_dup(oldfd);

    int host_fd = fd_to_host(oldfd);
    if (host_fd < 0) return -LINUX_EBADF;

    int new_host_fd = dup(host_fd);
    if (new_host_fd < 0) return linux_errno();

    int guest_fd = fd_alloc(fd_table[oldfd].type, new_host_fd);
    if (guest_fd < 0) {
        close(new_host_fd);
        return -LINUX_ENOMEM;
    }

    /* Create a fresh DIR* for directory FDs (see sys_dup3 comment) */
    if (fd_table[oldfd].type == FD_DIR) {
        int dir_fd = dup(new_host_fd);
        if (dir_fd >= 0) {
            DIR *dir = fdopendir(dir_fd);
            if (dir)
                fd_table[guest_fd].dir = dir;
            else
                close(dir_fd);
        }
    }

    return guest_fd;
}

int64_t sys_dup3(int oldfd, int newfd, int linux_flags) {
    if (oldfd < 0 || oldfd >= FD_TABLE_SIZE) return -LINUX_EBADF;
    if (newfd < 0 || newfd >= FD_TABLE_SIZE) return -LINUX_EBADF;
    /* Linux dup3(2): EINVAL if oldfd == newfd (unlike dup2 which is a no-op) */
    if (oldfd == newfd) return -LINUX_EINVAL;
    if (fd_table[oldfd].type == FD_CLOSED) return -LINUX_EBADF;
    /* Share the open-file description for descriptor-backed fds — see
     * the comment in sys_dup(). */
    if (fd_table[oldfd].desc)
        return hl_fd_dup3(oldfd, newfd, linux_flags);

    int host_fd = fd_to_host(oldfd);
    if (host_fd < 0) return -LINUX_EBADF;

    int new_host_fd = dup(host_fd);
    if (new_host_fd < 0) return linux_errno();

    /* fd_alloc_at cleans up old entry (incl. DIR* closedir) */
    fd_alloc_at(newfd, fd_table[oldfd].type, new_host_fd);
    fd_table[newfd].linux_flags = (linux_flags & LINUX_O_CLOEXEC);

    /* For directory FDs, create a fresh DIR* for the new entry.
     * dup() only duplicates the kernel fd — the C library DIR*
     * (used by our getdents64 emulation via readdir) must be
     * created separately to maintain independent directory state. */
    if (fd_table[oldfd].type == FD_DIR) {
        int dir_fd = dup(new_host_fd);
        if (dir_fd >= 0) {
            DIR *dir = fdopendir(dir_fd);
            if (dir)
                fd_table[newfd].dir = dir;
            else
                close(dir_fd);
        }
    }

    return newfd;
}

int64_t sys_fcntl(guest_t *g, int fd, int cmd, uint64_t arg) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -LINUX_EBADF;
    if (fd_table[fd].type == FD_CLOSED) return -LINUX_EBADF;

    /* F_GETFD/F_SETFD touch only descriptor state, so they must work even
     * for fds with no host alias (/dev/mixer installs host_fd == -1).
     * Checking fd_to_host() first made every fcntl on such a descriptor
     * fail with EBADF right after open() returned it successfully. */
    switch (cmd) {
    case 1: /* F_GETFD */
        return (fd_table[fd].linux_flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
    case 2: /* F_SETFD */
        if ((int)arg & LINUX_FD_CLOEXEC)
            fd_table[fd].linux_flags |= LINUX_O_CLOEXEC;
        else
            fd_table[fd].linux_flags &= ~LINUX_O_CLOEXEC;
        return 0;
    default:
        break;
    }

    /* Descriptor-backed fds keep the guest-visible status flags on the
     * open-file object. Their host alias is a dup() of an internal
     * transport (the audio producer socketpair) whose O_NONBLOCK hl sets
     * for its own purposes — reporting or mutating that leaked internal
     * state to the guest and left the guest's own O_NONBLOCK request
     * unseen by the OSS write path. */
    hl_open_file_t *of = fd_table[fd].of;
    /* ...but ONLY for those: a plain file or stdio also carries an open-file
     * object, and its status_flags are never seeded from the host, so
     * F_GETFL reported O_RDONLY for stdout and F_SETFL(O_NONBLOCK) never
     * reached the fd it was meant to affect. Those fall through to the host
     * path below and mirror the result back into status_flags. */
    if (of && of->ops != &hl_fd_ops_host_file && (cmd == 3 || cmd == 4)) {
        const uint32_t accmode = 0003; /* Linux O_ACCMODE */
        if (cmd == 3)
            return (int64_t)atomic_load(&of->status_flags);
        uint32_t cur = atomic_load(&of->status_flags);
        atomic_store(&of->status_flags,
                     (cur & accmode) | ((uint32_t)arg & ~accmode));
        return 0;
    }
    if (of && (cmd == 0 || cmd == 1030)) {
        /* hl_fd_dup() takes the lowest free fd and always clears CLOEXEC —
         * correct for dup(2), wrong for F_DUPFD, whose arg is the MINIMUM
         * acceptable fd, and for F_DUPFD_CLOEXEC, which must set it. */
        return hl_fd_dup_from(fd, (int)arg, cmd == 1030);
    }

    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Linux F_DUPFD=0, F_GETFD=1, F_SETFD=2, F_GETFL=3, F_SETFL=4,
     * F_DUPFD_CLOEXEC=1030 */
    switch (cmd) {
    case 0:    /* F_DUPFD */
    case 1030: /* F_DUPFD_CLOEXEC */ {
        int new_host = dup(host_fd);
        if (new_host < 0) return linux_errno();
        int gfd = fd_alloc_from((int)arg, fd_table[fd].type, new_host);
        if (gfd < 0) { close(new_host); return -LINUX_EMFILE; }
        fd_table[gfd].linux_flags = fd_table[fd].linux_flags & ~LINUX_O_CLOEXEC;
        if (cmd == 1030)
            fd_table[gfd].linux_flags |= LINUX_O_CLOEXEC;
        /* Create fresh DIR* for directory FDs (see sys_dup3 comment) */
        if (fd_table[fd].type == FD_DIR) {
            int dir_fd = dup(new_host);
            if (dir_fd >= 0) {
                DIR *dir = fdopendir(dir_fd);
                if (dir)
                    fd_table[gfd].dir = dir;
                else
                    close(dir_fd);
            }
        }
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
    case 3: { /* F_GETFL */
        int mac_fl = fcntl(host_fd, F_GETFL);
        if (mac_fl < 0) return linux_errno();
        return mac_to_linux_status_flags(mac_fl);
    }
    case 4: /* F_SETFL */
        if (fcntl(host_fd, F_SETFL, linux_to_mac_status_flags((int)arg)) < 0)
            return linux_errno();
        /* Keep the open-file view in step, so code reading status_flags
         * (the OSS write path, fork export) does not disagree with the fd. */
        if (of) {
            const uint32_t accmode = 0003;
            uint32_t cur = atomic_load(&of->status_flags);
            atomic_store(&of->status_flags,
                         (cur & accmode) | ((uint32_t)arg & ~accmode));
        }
        return 0;
    case 5:  /* F_GETLK */
    case 6:  /* F_SETLK */
    case 7: { /* F_SETLKW */
        /* Translate Linux struct flock (aarch64) to macOS struct flock.
         * Linux aarch64 layout: {short l_type, short l_whence,
         *   long l_start, long l_len, int l_pid, pad[4]}
         * macOS layout: {off_t l_start, off_t l_len, pid_t l_pid,
         *   short l_type, short l_whence}
         * Use guest_read/guest_write (not guest_ptr) to safely handle
         * structs that span 2MB page table block boundaries. */
        uint8_t lflock[32];  /* Linux struct flock is 32 bytes on aarch64 */
        if (guest_read(g, arg, lflock, sizeof(lflock)) < 0)
            return -LINUX_EFAULT;

        /* Read Linux flock fields */
        int16_t l_type, l_whence;
        int64_t l_start, l_len;
        memcpy(&l_type,   lflock + 0, 2);
        memcpy(&l_whence, lflock + 2, 2);
        memcpy(&l_start,  lflock + 8, 8);   /* offset 8 due to padding */
        memcpy(&l_len,    lflock + 16, 8);

        struct flock mac_fl = {
            .l_start  = l_start,
            .l_len    = l_len,
            .l_pid    = 0,
            .l_type   = l_type,   /* F_RDLCK=0, F_WRLCK=1, F_UNLCK=2 same on both */
            .l_whence = l_whence, /* SEEK_SET=0, SEEK_CUR=1, SEEK_END=2 same */
        };

        int mac_cmd = (cmd == 5) ? F_GETLK : (cmd == 6) ? F_SETLK : F_SETLKW;
        if (fcntl(host_fd, mac_cmd, &mac_fl) < 0)
            return linux_errno();

        /* For F_GETLK, write back the result */
        if (cmd == 5) {
            int16_t rt = mac_fl.l_type, rw = mac_fl.l_whence;
            int64_t rs = mac_fl.l_start, rl = mac_fl.l_len;
            int32_t rp = mac_fl.l_pid;
            memset(lflock, 0, sizeof(lflock));
            memcpy(lflock + 0, &rt, 2);
            memcpy(lflock + 2, &rw, 2);
            memcpy(lflock + 8, &rs, 8);
            memcpy(lflock + 16, &rl, 8);
            memcpy(lflock + 24, &rp, 4);
            if (guest_write(g, arg, lflock, sizeof(lflock)) < 0)
                return -LINUX_EFAULT;
        }
        return 0;
    }
    case 1024: /* F_GETPIPE_SZ */
        /* macOS doesn't support pipe size queries; return default 64KB */
        return 65536;
    case 1031: /* F_SETPIPE_SZ */
        /* macOS doesn't support pipe size setting; pretend success */
        return (int64_t)arg;
    default:
        return -LINUX_EINVAL;
    }
}

#define LINUX_CLOSE_RANGE_CLOEXEC 4

int64_t sys_close_range(unsigned int first, unsigned int last,
                        unsigned int flags) {
    /* Linux returns EINVAL when first > last (even if both are valid) */
    if (first > last) return -LINUX_EINVAL;
    /* Reject unknown flags */
    if (flags & ~(unsigned)LINUX_CLOSE_RANGE_CLOEXEC) return -LINUX_EINVAL;
    /* Clamp to FD table size (Linux clamps ~0U to NR_OPEN_MAX) */
    if (last >= (unsigned)FD_TABLE_SIZE) last = FD_TABLE_SIZE - 1;

    /* CLOSE_RANGE_CLOEXEC: mark FDs as CLOEXEC without closing them.
     * Hold fd_lock to prevent races with concurrent fd_alloc/close. */
    if (flags & LINUX_CLOSE_RANGE_CLOEXEC) {
        pthread_mutex_lock(&fd_lock);
        for (unsigned int i = first; i <= last && i < (unsigned)FD_TABLE_SIZE; i++) {
            if (fd_table[i].type != FD_CLOSED)
                fd_table[i].linux_flags |= LINUX_O_CLOEXEC;
        }
        pthread_mutex_unlock(&fd_lock);
        return 0;
    }

    for (unsigned int i = first; i <= last && i < (unsigned)FD_TABLE_SIZE; i++) {
        fd_entry_t snap;
        if (!fd_snapshot_and_close((int)i, &snap))
            continue;

        /* Clean up emulated I/O subsystem state */
        switch (snap.type) {
        case FD_EVENTFD:  eventfd_close((int)i);  break;
        case FD_SIGNALFD: signalfd_close((int)i); break;
        case FD_TIMERFD:  timerfd_close((int)i);  break;
        case FD_INOTIFY:  inotify_close((int)i);  break;
        default: break;
        }

        /* fd_snapshot_and_close transferred ownership of desc/of to us.
         * Releasing the descriptor runs the open-file destroy op (e.g.
         * oss_destroy -> hl_audio_stream_destroy) and closes the host
         * alias; skipping it leaks the object graph and leaves the audio
         * queue playing after the guest closed the device. */
        if (snap.desc) {
            hl_descriptor_release(snap.desc);
            continue;
        }

        if (snap.dir) {
            if (snap.type == FD_DIR)
                closedir((DIR *)snap.dir);
            else if (snap.type == FD_EPOLL)
                free(snap.dir);
        }
        if (snap.of)
            hl_open_file_release(snap.of);

        if (snap.type != FD_STDIO && snap.host_fd >= 0)
            close(snap.host_fd);
    }
    return 0;
}

/* ---------- directory operations ---------- */

/* getdents64: read directory entries from a guest directory fd.
 * Uses the persistent DIR* stored in fd_table (created by openat). */
int64_t sys_getdents64(guest_t *g, int fd, uint64_t buf_gva, uint64_t count) {
    if (fd < 0 || fd >= FD_TABLE_SIZE) return -LINUX_EBADF;
    if (fd_table[fd].type == FD_CLOSED) return -LINUX_EBADF;

    if (fd_table[fd].type == FD_VIRTUAL_DIR) {
        hl_vdir_t *vd = (hl_vdir_t *)fd_table[fd].dir;
        if (!vd) return -LINUX_ENOTDIR;
        if (!guest_ptr(g, buf_gva)) return -LINUX_EFAULT;
        size_t pos = 0;
        uint8_t ent[280];
        /* "." and ".." first, then one entry per child mount component. */
        while (vd->pos < vd->n + 2) {
            const char *name = (vd->pos == 0) ? "."
                             : (vd->pos == 1) ? ".."
                             : vd->names[vd->pos - 2];
            size_t nl = strlen(name);
            size_t reclen = (19 + nl + 1 + 7) & ~7ULL;
            if (pos + reclen > count) break;
            linux_dirent64_t lde;
            lde.d_ino    = (uint64_t)(vd->pos + 1);
            lde.d_off    = (int64_t)(vd->pos + 1);
            lde.d_reclen = (uint16_t)reclen;
            lde.d_type   = DT_DIR;
            memcpy(ent, &lde, sizeof(lde));
            memcpy(ent + 19, name, nl + 1);
            if (19 + nl + 1 < reclen)
                memset(ent + 19 + nl + 1, 0, reclen - (19 + nl + 1));
            if (guest_write(g, buf_gva + pos, ent, reclen) < 0)
                return pos > 0 ? (int64_t)pos : -LINUX_EFAULT;
            pos += reclen;
            vd->pos++;
        }
        return (int64_t)pos;
    }

    DIR *dir = (DIR *)fd_table[fd].dir;
    if (!dir) return -LINUX_ENOTDIR;

    if (!guest_ptr(g, buf_gva)) return -LINUX_EFAULT;

    size_t guest_pos = 0;
    struct dirent *de;

    /* Temp buffer for dirent serialization — max dirent64 is 280 bytes
     * (19-byte header + NAME_MAX=255 + null + padding to 8). Using a
     * stack buffer avoids guest_ptr boundary issues: guest_write() handles
     * 2MB block crossings that raw memcpy into guest_ptr() cannot. */
    uint8_t entry_buf[280];

    while (1) {
        /* Save position BEFORE readdir so we can rewind if the entry
         * doesn't fit.  macOS telldir returns an opaque cookie —
         * arithmetic on it (e.g. telldir()-1) is undefined. */
        long saved_pos = telldir(dir);
        de = readdir(dir);
        if (!de) break;

        size_t name_len = strlen(de->d_name);
        /* entry_buf is 280 bytes and macOS d_name is __DARWIN_MAXPATHLEN
         * (1024), so strlen() can exceed what the record can hold. Skip
         * over-long names rather than overflowing the stack buffer.
         * Unreachable on APFS/HFS+ (NAME_MAX 255) but the rooted profile
         * can bind arbitrary mounted filesystems. */
        if (name_len > 255) continue;
        /* Linux dirent64: 19-byte header + name + null, padded to 8 */
        size_t reclen = (19 + name_len + 1 + 7) & ~7ULL;

        if (guest_pos + reclen > count) {
            /* Entry doesn't fit — rewind so next call gets it */
            seekdir(dir, saved_pos);
            break;
        }

        linux_dirent64_t lde;
        lde.d_ino = de->d_ino;
        lde.d_off = telldir(dir);
        lde.d_reclen = (uint16_t)reclen;
        lde.d_type = de->d_type;

        /* Serialize entry into temp buffer, then copy to guest via
         * guest_write() which handles 2MB block boundary crossings. */
        memcpy(entry_buf, &lde, sizeof(lde));
        memcpy(entry_buf + 19, de->d_name, name_len + 1);
        size_t pad_start = 19 + name_len + 1;
        if (pad_start < reclen)
            memset(entry_buf + pad_start, 0, reclen - pad_start);

        if (guest_write(g, buf_gva + guest_pos, entry_buf, reclen) < 0)
            return guest_pos > 0 ? (int64_t)guest_pos : -LINUX_EFAULT;

        guest_pos += reclen;
    }

    return (int64_t)guest_pos;
}

int64_t sys_chdir(guest_t *g, uint64_t path_gva) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;
    return hl_vfs_chdir(path);
}

int64_t sys_fchdir(int fd) {
    return hl_vfs_fchdir(fd);
}

int64_t sys_chroot(guest_t *g, uint64_t path_gva) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;
    /* Emulate chroot by updating the sysroot prefix.  All path resolution
     * already redirects through sysroot.  chroot("/") is a no-op — the
     * guest already sees "/" as root, and resetting sysroot to "/" would
     * break dynamic linker resolution.  Real chroot() requires root and
     * doesn't make sense in hl's single-process VM.  This enables
     * coreutils stdbuf (which does fork → chroot("/") → exec) and the
     * chroot coreutil itself. */
    if (strcmp(path, "/") != 0 && hl_vfs_mode() == HL_FS_ROOTED) {
        /* Resolve first: storing an unresolved guest path as the host
         * sysroot prefix corrupted every later absolute resolution. */
        hl_vfs_resolve_t r;
        int rc = hl_vfs_resolve_at(LINUX_AT_FDCWD, path, 1, 0, &r);
        if (rc < 0) return rc;
        struct stat rst;
        if (stat(r.host_path, &rst) < 0) return linux_errno();
        if (!S_ISDIR(rst.st_mode)) return -LINUX_ENOTDIR;
        proc_set_sysroot(r.host_path);
        return 0;
    }
    if (strcmp(path, "/") != 0) {
        struct stat st;
        if (stat(path, &st) < 0)
            return linux_errno();
        if (!S_ISDIR(st.st_mode))
            return -LINUX_ENOTDIR;
        proc_set_sysroot(path);
    }
    return 0;
}

/* ---------- pipe/seek ---------- */

int64_t sys_pipe2(guest_t *g, uint64_t fds_gva, int linux_flags) {
    int host_fds[2];
    if (pipe(host_fds) < 0)
        return linux_errno();

    int guest_fds[2];
    guest_fds[0] = fd_alloc(FD_PIPE, host_fds[0]);
    guest_fds[1] = fd_alloc(FD_PIPE, host_fds[1]);

    if (guest_fds[0] < 0 || guest_fds[1] < 0) {
        if (guest_fds[0] >= 0) fd_mark_closed(guest_fds[0]);
        if (guest_fds[1] >= 0) fd_mark_closed(guest_fds[1]);
        close(host_fds[0]);
        close(host_fds[1]);
        return -LINUX_ENOMEM;
    }

    /* Apply O_NONBLOCK to host FDs if requested */
    if (linux_flags & LINUX_O_NONBLOCK) {
        fcntl(host_fds[0], F_SETFL, O_NONBLOCK);
        fcntl(host_fds[1], F_SETFL, O_NONBLOCK);
    }

    /* Propagate O_CLOEXEC if set in flags */
    fd_table[guest_fds[0]].linux_flags = linux_flags & LINUX_O_CLOEXEC;
    fd_table[guest_fds[1]].linux_flags = linux_flags & LINUX_O_CLOEXEC;

    int32_t fds[2] = { guest_fds[0], guest_fds[1] };
    if (guest_write(g, fds_gva, fds, sizeof(fds)) < 0) {
        fd_mark_closed(guest_fds[0]);
        fd_mark_closed(guest_fds[1]);
        close(host_fds[0]);
        close(host_fds[1]);
        return -LINUX_EFAULT;
    }

    return 0;
}

int64_t sys_lseek(int fd, int64_t offset, int whence) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    /* Directory FDs: getdents64 is backed by a host DIR* (fdopendir).
     * Guest musl rewinddir() is just lseek(fd, 0, SEEK_SET) plus
     * clearing its own buffer. lseek on the host fd does NOT reset
     * the DIR* stream — readdir() would keep returning NULL after the
     * first full scan. GTK 1.2 file selection (and similar) does a
     * two-pass readdir (count, rewind, re-read); without this, the
     * second pass is empty → open_dir fails → infinite tryagain on
     * "/" → UI hang/freeze on "Add file". */
    if (fd >= 0 && fd < FD_TABLE_SIZE &&
        fd_table[fd].type == FD_DIR && fd_table[fd].dir) {
        DIR *dir = (DIR *)fd_table[fd].dir;
        if (whence == SEEK_SET && offset == 0) {
            rewinddir(dir);
            return 0;
        }
        if (whence == SEEK_CUR && offset == 0) {
            /* telldir cookie — opaque; musl may probe position */
            return (int64_t)telldir(dir);
        }
        if (whence == SEEK_SET) {
            /* Opaque d_off from a prior getdents64 d_off field */
            seekdir(dir, (long)offset);
            return offset;
        }
        /* SEEK_END / SEEK_CUR nonzero: not meaningful for DIR* */
        return -LINUX_EINVAL;
    }

    off_t ret = lseek(host_fd, offset, whence);
    return ret < 0 ? linux_errno() : (int64_t)ret;
}

/* ---------- path operations ---------- */

int64_t sys_readlinkat(guest_t *g, int dirfd, uint64_t path_gva,
                       uint64_t buf_gva, uint64_t bufsiz) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    /* Intercept /proc paths (e.g. /proc/self/exe, /proc/self/fd/N) */
    char link[LINUX_PATH_MAX];
    int intercepted = proc_intercept_readlink(path, link, sizeof(link));
    if (intercepted >= 0) {
        size_t copy_len = (size_t)intercepted < bufsiz
                          ? (size_t)intercepted : bufsiz;
        if (guest_write(g, buf_gva, link, copy_len) < 0)
            return -LINUX_EFAULT;
        return (int64_t)copy_len;
    }
    if (intercepted == -1) {
        return linux_errno();
    }
    /* intercepted == -2: not intercepted, fall through */

    char hostbuf[LINUX_PATH_MAX];
    const char *read_path = NULL;
    int rr = resolve_path_for_op_ex(dirfd, path, 0, 0, hostbuf, sizeof(hostbuf),
                                 &read_path, NULL);
    if (rr < 0) return rr;
    int host_dirfd = (hl_vfs_mode() == HL_FS_ROOTED)
        ? host_dirfd_for_resolved()
        : ((dirfd == LINUX_AT_FDCWD) ? AT_FDCWD : fd_to_host(dirfd));
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD && hl_vfs_mode() != HL_FS_ROOTED)
        return -LINUX_EBADF;

    ssize_t len = readlinkat(host_dirfd, read_path, link, sizeof(link) - 1);
    if (len < 0) return linux_errno();

    size_t copy_len = (size_t)len < bufsiz ? (size_t)len : bufsiz;
    if (guest_write(g, buf_gva, link, copy_len) < 0)
        return -LINUX_EFAULT;

    return (int64_t)copy_len;
}

int64_t sys_unlinkat(guest_t *g, int dirfd, uint64_t path_gva, int flags) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *host_path = NULL;
    const hl_mount_t *mnt = NULL;
    int rr = resolve_path_for_op_ex(dirfd, path, 1, 0, hostbuf, sizeof(hostbuf),
                                 &host_path, &mnt);
    if (rr < 0) return rr;
    if (mnt && (mnt->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL))) return -LINUX_EROFS;
    if (mnt && (mnt->flags & HL_MOUNT_VIRTUAL)) return -LINUX_EPERM;

    int host_dirfd = (hl_vfs_mode() == HL_FS_ROOTED)
        ? host_dirfd_for_resolved()
        : ((dirfd == LINUX_AT_FDCWD) ? AT_FDCWD : fd_to_host(dirfd));
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD && hl_vfs_mode() != HL_FS_ROOTED)
        return -LINUX_EBADF;

    int host_flags = translate_at_flags(flags);
    if (unlinkat(host_dirfd, host_path, host_flags) < 0)
        return linux_errno();

    return 0;
}

int64_t sys_mkdirat(guest_t *g, int dirfd, uint64_t path_gva, int mode) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *host_path = NULL;
    const hl_mount_t *mnt = NULL;
    int rr = resolve_path_for_op_ex(dirfd, path, 1, 0, hostbuf, sizeof(hostbuf),
                                 &host_path, &mnt);
    if (rr < 0) return rr;
    if (mnt && (mnt->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL))) return -LINUX_EROFS;
    if (mnt && (mnt->flags & HL_MOUNT_VIRTUAL)) return -LINUX_EPERM;

    int host_dirfd = (hl_vfs_mode() == HL_FS_ROOTED)
        ? host_dirfd_for_resolved()
        : ((dirfd == LINUX_AT_FDCWD) ? AT_FDCWD : fd_to_host(dirfd));
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD && hl_vfs_mode() != HL_FS_ROOTED)
        return -LINUX_EBADF;

    if (mkdirat(host_dirfd, host_path, (mode_t)mode) < 0)
        return linux_errno();

    return 0;
}

/* Linux RENAME_* flags for renameat2 */
#define LINUX_RENAME_NOREPLACE  (1 << 0)
#define LINUX_RENAME_EXCHANGE   (1 << 1)

int64_t sys_renameat2(guest_t *g, int olddirfd, uint64_t oldpath_gva,
                      int newdirfd, uint64_t newpath_gva, int flags) {
    char oldpath[LINUX_PATH_MAX], newpath[LINUX_PATH_MAX];
    if (guest_read_str(g, oldpath_gva, oldpath, sizeof(oldpath)) < 0)
        return -LINUX_EFAULT;
    if (guest_read_str(g, newpath_gva, newpath, sizeof(newpath)) < 0)
        return -LINUX_EFAULT;

    char oldhost[LINUX_PATH_MAX], newhost[LINUX_PATH_MAX];
    const char *oh = oldpath, *nh = newpath;
    const hl_mount_t *om = NULL, *nm = NULL;
    int host_olddir, host_newdir;

    /* Resolve in BOTH modes — see the note in sys_fchmodat(). */
    {
        int r1 = resolve_path_for_op_ex(olddirfd, oldpath, 0, 0, oldhost,
                                     sizeof(oldhost), &oh, &om);
        if (r1 < 0) return r1;
        int r2 = resolve_path_for_op_ex(newdirfd, newpath, 1, 0, newhost,
                                     sizeof(newhost), &nh, &nm);
        if (r2 < 0) return r2;
        if (om && nm && om->id != nm->id) return -LINUX_EXDEV;
        if ((om && (om->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL))) || (nm && (nm->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL))))
            return -LINUX_EROFS;
        host_olddir = host_dirfd_for_op(olddirfd);
        host_newdir = host_dirfd_for_op(newdirfd);
        if (host_olddir == -1 || host_newdir == -1) return -LINUX_EBADF;
    }

    /* RENAME_NOREPLACE: fail if destination exists. macOS renamex_np
     * supports RENAME_EXCL for the same semantics. Only supported for
     * AT_FDCWD paths (renamex_np doesn't take dirfd arguments). */
    if (flags & LINUX_RENAME_NOREPLACE) {
        if (host_olddir == AT_FDCWD && host_newdir == AT_FDCWD) {
            if (renamex_np(oh, nh, RENAME_EXCL) < 0)
                return linux_errno();
            return 0;
        }
        if (linkat(host_olddir, oh, host_newdir, nh, 0) < 0)
            return linux_errno();
        unlinkat(host_olddir, oh, 0);
        return 0;
    }

    if (flags & LINUX_RENAME_EXCHANGE) {
        if (host_olddir == AT_FDCWD && host_newdir == AT_FDCWD) {
            if (renamex_np(oh, nh, RENAME_SWAP) < 0)
                return linux_errno();
            return 0;
        }
        return -LINUX_EINVAL;
    }

    if (renameat(host_olddir, oh, host_newdir, nh) < 0)
        return linux_errno();

    return 0;
}

int64_t sys_mknodat(guest_t *g, int dirfd, uint64_t path_gva,
                    int mode, int dev) {
    (void)dev;
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *host_path = NULL;
    const hl_mount_t *mnt = NULL;
    int rr = resolve_path_for_op_ex(dirfd, path, 1, 0, hostbuf, sizeof(hostbuf),
                                 &host_path, &mnt);
    if (rr < 0) return rr;
    if (mnt && (mnt->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL))) return -LINUX_EROFS;

    int host_dirfd = (hl_vfs_mode() == HL_FS_ROOTED)
        ? host_dirfd_for_resolved() : resolve_dirfd(dirfd);
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD && hl_vfs_mode() != HL_FS_ROOTED)
        return -LINUX_EBADF;

    if (S_ISFIFO(mode)) {
        if (mkfifoat(host_dirfd, host_path, mode & 0777) < 0)
            return linux_errno();
        return 0;
    }

    if (S_ISREG(mode) || (mode & S_IFMT) == 0) {
        int fd = openat(host_dirfd, host_path, O_CREAT | O_WRONLY | O_EXCL,
                        mode & 0777);
        if (fd < 0) return linux_errno();
        close(fd);
        return 0;
    }

    return -LINUX_ENOSYS;
}

int64_t sys_symlinkat(guest_t *g, uint64_t target_gva,
                      int dirfd, uint64_t linkpath_gva) {
    char target[LINUX_PATH_MAX], linkpath[LINUX_PATH_MAX];
    if (guest_read_str(g, target_gva, target, sizeof(target)) < 0)
        return -LINUX_EFAULT;
    if (guest_read_str(g, linkpath_gva, linkpath, sizeof(linkpath)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *host_link = NULL;
    const hl_mount_t *mnt = NULL;
    int rr = resolve_path_for_op_ex(dirfd, linkpath, 1, 0, hostbuf, sizeof(hostbuf),
                                 &host_link, &mnt);
    if (rr < 0) return rr;
    if (mnt && (mnt->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL))) return -LINUX_EROFS;

    int host_dirfd = (hl_vfs_mode() == HL_FS_ROOTED)
        ? host_dirfd_for_resolved() : resolve_dirfd(dirfd);
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD && hl_vfs_mode() != HL_FS_ROOTED)
        return -LINUX_EBADF;

    /* target is stored as-is (guest path semantics for absolute symlinks) */
    if (symlinkat(target, host_dirfd, host_link) < 0)
        return linux_errno();

    return 0;
}

int64_t sys_linkat(guest_t *g, int olddirfd, uint64_t oldpath_gva,
                   int newdirfd, uint64_t newpath_gva, int flags) {
    char oldpath[LINUX_PATH_MAX], newpath[LINUX_PATH_MAX];
    if (guest_read_str(g, oldpath_gva, oldpath, sizeof(oldpath)) < 0)
        return -LINUX_EFAULT;
    if (guest_read_str(g, newpath_gva, newpath, sizeof(newpath)) < 0)
        return -LINUX_EFAULT;

    char oldhost[LINUX_PATH_MAX], newhost[LINUX_PATH_MAX];
    const char *oh = oldpath, *nh = newpath;
    const hl_mount_t *om = NULL, *nm = NULL;
    int host_olddir, host_newdir;

    /* Resolve in BOTH modes — see the note in sys_fchmodat(). */
    {
        int r1 = resolve_path_for_op_ex(olddirfd, oldpath, 0, (flags & LINUX_AT_SYMLINK_FOLLOW) != 0, oldhost,
                                     sizeof(oldhost), &oh, &om);
        if (r1 < 0) return r1;
        int r2 = resolve_path_for_op_ex(newdirfd, newpath, 1, 0, newhost,
                                     sizeof(newhost), &nh, &nm);
        if (r2 < 0) return r2;
        if (om && nm && om->id != nm->id) return -LINUX_EXDEV;
        if (nm && (nm->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL))) return -LINUX_EROFS;
        host_olddir = host_dirfd_for_op(olddirfd);
        host_newdir = host_dirfd_for_op(newdirfd);
        if (host_olddir == -1 || host_newdir == -1) return -LINUX_EBADF;
    }

    int mac_flags = translate_at_flags(flags);
    if (linkat(host_olddir, oh, host_newdir, nh, mac_flags) < 0)
        return linux_errno();

    return 0;
}

int64_t sys_faccessat(guest_t *g, int dirfd, uint64_t path_gva,
                      int mode, int flags) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    if (hl_device_lookup(path))
        return 0;

    struct stat dummy_st;
    if (proc_intercept_stat(path, &dummy_st) == 0)
        return 0;

    char hostbuf[LINUX_PATH_MAX];
    const char *check_path = NULL;
    int rr = resolve_path_for_op_ex(dirfd, path, 0, !(flags & LINUX_AT_SYMLINK_NOFOLLOW), hostbuf, sizeof(hostbuf),
                                 &check_path, NULL);
    if (rr < 0) return rr;

    int host_dirfd = (hl_vfs_mode() == HL_FS_ROOTED)
        ? host_dirfd_for_resolved()
        : ((dirfd == LINUX_AT_FDCWD) ? AT_FDCWD : fd_to_host(dirfd));
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD && hl_vfs_mode() != HL_FS_ROOTED)
        return -LINUX_EBADF;

    int mac_flags = translate_faccessat_flags(flags);
    if (faccessat(host_dirfd, check_path, mode, mac_flags) < 0)
        return linux_errno();

    return 0;
}

/* ---------- truncate ---------- */

int64_t sys_ftruncate(int fd, int64_t length) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (ftruncate(host_fd, length) < 0)
        return linux_errno();
    return 0;
}

int64_t sys_truncate(guest_t *g, uint64_t path_gva, int64_t length) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *trunc_path = NULL;
    const hl_mount_t *mnt = NULL;
    int rr = resolve_path_for_op(LINUX_AT_FDCWD, path, 0, hostbuf,
                                 sizeof(hostbuf), &trunc_path, &mnt);
    if (rr < 0) return rr;
    if (mnt && (mnt->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL))) return -LINUX_EROFS;

    if (truncate(trunc_path, length) < 0)
        return linux_errno();
    return 0;
}

/* ---------- permissions/ownership ---------- */

int64_t sys_fchmod(int fd, uint32_t mode) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (fchmod(host_fd, mode) < 0)
        return linux_errno();
    return 0;
}

int64_t sys_fchmodat(guest_t *g, int dirfd, uint64_t path_gva,
                     uint32_t mode, int flags) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *host_path = path;
    const hl_mount_t *mnt = NULL;
    int host_dirfd;
    {
        int rr = resolve_path_for_op_ex(dirfd, path, 0, !(flags & LINUX_AT_SYMLINK_NOFOLLOW), hostbuf, sizeof(hostbuf),
                                     &host_path, &mnt);
        if (rr < 0) return rr;
        if (mnt && (mnt->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL))) return -LINUX_EROFS;
        host_dirfd = host_dirfd_for_op(dirfd);
        if (host_dirfd == -1) return -LINUX_EBADF;
    }

    /* macOS fchmodat doesn't support AT_SYMLINK_NOFOLLOW */
    int mac_flags = 0;
    if (flags & LINUX_AT_SYMLINK_NOFOLLOW) {
        /* Best effort: use lstat to check if it's a symlink */
        struct stat st;
        if (fstatat(host_dirfd, host_path, &st, AT_SYMLINK_NOFOLLOW) == 0
            && S_ISLNK(st.st_mode)) {
            return -LINUX_EOPNOTSUPP;
        }
    }

    if (fchmodat(host_dirfd, host_path, mode, mac_flags) < 0)
        return linux_errno();

    return 0;
}

int64_t sys_fchownat(guest_t *g, int dirfd, uint64_t path_gva,
                     uint32_t owner, uint32_t group, int flags) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *host_path = path;
    const hl_mount_t *mnt = NULL;
    int host_dirfd;
    {
        int rr = resolve_path_for_op_ex(dirfd, path, 0, !(flags & LINUX_AT_SYMLINK_NOFOLLOW), hostbuf, sizeof(hostbuf),
                                     &host_path, &mnt);
        if (rr < 0) return rr;
        if (mnt && (mnt->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL))) return -LINUX_EROFS;
        host_dirfd = host_dirfd_for_op(dirfd);
        if (host_dirfd == -1) return -LINUX_EBADF;
    }

    int mac_flags = translate_at_flags(flags);
    if (fchownat(host_dirfd, host_path, owner, group, mac_flags) < 0)
        return linux_errno();

    return 0;
}

int64_t sys_fchown(int fd, uint32_t owner, uint32_t group) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;
    if (fchown(host_fd, owner, group) < 0)
        return linux_errno();
    return 0;
}

int64_t sys_utimensat(guest_t *g, int dirfd, uint64_t path_gva,
                      uint64_t times_gva, int flags) {
    int host_dirfd = resolve_dirfd(dirfd);
    if (host_dirfd < 0 && dirfd != LINUX_AT_FDCWD
        && hl_vfs_mode() != HL_FS_ROOTED)
        return -LINUX_EBADF;

    /* If path is NULL (path_gva == 0), operate on the dirfd itself */
    const char *path_arg = NULL;
    char path[LINUX_PATH_MAX];
    char hostbuf[LINUX_PATH_MAX];
    if (path_gva != 0) {
        if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
            return -LINUX_EFAULT;
        /* Resolve in BOTH modes: the legacy branch passed the raw guest
         * path while open/stat/unlink applied the sysroot redirect. */
        {
            const hl_mount_t *mnt = NULL;
            int rr = resolve_path_for_op_ex(dirfd, path, 0,
                                            !(flags & LINUX_AT_SYMLINK_NOFOLLOW),
                                            hostbuf, sizeof(hostbuf),
                                            &path_arg, &mnt);
            if (rr < 0) return rr;
            /* utimensat writes metadata; it never asked for the mount, so a
             * read-only bind could still have its timestamps rewritten. */
            if (mnt && (mnt->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL)))
                return -LINUX_EROFS;
            host_dirfd = host_dirfd_for_op(dirfd);
            if (host_dirfd == -1) return -LINUX_EBADF;
        }
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

    /* macOS utimensat() does not support NULL path (Linux extension).
     * When path is NULL, the caller wants to operate on dirfd itself,
     * so use futimens() instead. */
    if (path_arg == NULL) {
        if (futimens(host_dirfd, times_gva ? ts : NULL) < 0)
            return linux_errno();
    } else {
        if (utimensat(host_dirfd, path_arg, times_gva ? ts : NULL, mac_flags) < 0)
            return linux_errno();
    }

    return 0;
}

/* ---------- xattr syscalls ---------- */

/* macOS xattr API has extra `position` and `options` parameters vs Linux.
 * Linux: getxattr(path, name, value, size)
 * macOS: getxattr(path, name, value, size, position, options)
 * We pass position=0, options=0 for normal operation, and
 * options=XATTR_NOFOLLOW for lgetxattr/lsetxattr/etc. */

int64_t sys_getxattr(guest_t *g, uint64_t path_gva, uint64_t name_gva,
                     uint64_t value_gva, uint64_t size, int nofollow) {
    char path[4096], name[256];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;
    if (guest_read_str(g, name_gva, name, sizeof(name)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *hpath = path;
    /* follow = !nofollow. Resolving through the final symlink handed the
     * TARGET's host path to a call that then asked for XATTR_NOFOLLOW,
     * so every l*xattr variant behaved exactly like its non-l form. */
    int rr = resolve_path_for_op_ex(LINUX_AT_FDCWD, path, 0, !nofollow,
                                    hostbuf, sizeof(hostbuf), &hpath,
                                    NULL);
    if (rr < 0) return rr;

    int opts = nofollow ? XATTR_NOFOLLOW : 0;

    if (size == 0) {
        /* Size query */
        ssize_t ret = getxattr(hpath, name, NULL, 0, 0, opts);
        return ret < 0 ? linux_errno() : ret;
    }

    /* Use a host-side buffer to avoid guest_ptr spanning 2MB block
     * boundaries.  Cap at 64KB (Linux XATTR_SIZE_MAX). */
    if (size > 65536) return -LINUX_E2BIG;
    void *buf = malloc(size);
    if (!buf) return -LINUX_ENOMEM;

    ssize_t ret = getxattr(hpath, name, buf, size, 0, opts);
    if (ret < 0) {
        free(buf);
        return linux_errno();
    }
    if (guest_write(g, value_gva, buf, (size_t)ret) < 0) {
        free(buf);
        return -LINUX_EFAULT;
    }
    free(buf);
    return ret;
}

int64_t sys_setxattr(guest_t *g, uint64_t path_gva, uint64_t name_gva,
                     uint64_t value_gva, uint64_t size, int flags,
                     int nofollow) {
    char path[4096], name[256];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;
    if (guest_read_str(g, name_gva, name, sizeof(name)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *hpath = path;
    const hl_mount_t *mnt = NULL;
    /* follow = !nofollow. Resolving through the final symlink handed the
     * TARGET's host path to a call that then asked for XATTR_NOFOLLOW,
     * so every l*xattr variant behaved exactly like its non-l form. */
    int rr = resolve_path_for_op_ex(LINUX_AT_FDCWD, path, 0, !nofollow,
                                    hostbuf, sizeof(hostbuf), &hpath,
                                    &mnt);
    if (rr < 0) return rr;
    /* setxattr writes; a read-only or virtual mount must refuse it. The
     * mount was never even requested here, so every RO bind stayed
     * writable through extended attributes. */
    if (mnt && (mnt->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL)))
        return -LINUX_EROFS;

    /* Use host-side buffer to safely read from guest memory. */
    if (size > 65536) return -LINUX_E2BIG;
    void *buf = NULL;
    if (size > 0) {
        buf = malloc(size);
        if (!buf) return -LINUX_ENOMEM;
        if (guest_read(g, value_gva, buf, size) < 0) {
            free(buf);
            return -LINUX_EFAULT;
        }
    }

    int opts = nofollow ? XATTR_NOFOLLOW : 0;
    /* Linux flags: XATTR_CREATE=1, XATTR_REPLACE=2 — same on macOS */
    if (flags & 1) opts |= XATTR_CREATE;
    if (flags & 2) opts |= XATTR_REPLACE;

    int ret = setxattr(hpath, name, buf, size, 0, opts);
    free(buf);
    return ret < 0 ? linux_errno() : 0;
}

int64_t sys_listxattr(guest_t *g, uint64_t path_gva,
                      uint64_t list_gva, uint64_t size, int nofollow) {
    char path[LINUX_PATH_MAX];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *hpath = path;
    /* follow = !nofollow. Resolving through the final symlink handed the
     * TARGET's host path to a call that then asked for XATTR_NOFOLLOW,
     * so every l*xattr variant behaved exactly like its non-l form. */
    int rr = resolve_path_for_op_ex(LINUX_AT_FDCWD, path, 0, !nofollow,
                                    hostbuf, sizeof(hostbuf), &hpath,
                                    NULL);
    if (rr < 0) return rr;

    int opts = nofollow ? XATTR_NOFOLLOW : 0;

    if (size == 0) {
        ssize_t ret = listxattr(hpath, NULL, 0, opts);
        return ret < 0 ? linux_errno() : ret;
    }

    /* Bounce buffer: guest region may span a 2MB block boundary. */
    char *buf = malloc(size);
    if (!buf) return -LINUX_ENOMEM;

    ssize_t ret = listxattr(hpath, buf, size, opts);
    if (ret >= 0 && guest_write(g, list_gva, buf, ret) < 0)
        ret = -LINUX_EFAULT;
    else if (ret < 0)
        ret = linux_errno();
    free(buf);
    return ret;
}

int64_t sys_removexattr(guest_t *g, uint64_t path_gva,
                        uint64_t name_gva, int nofollow) {
    char path[4096], name[256];
    if (guest_read_str(g, path_gva, path, sizeof(path)) < 0)
        return -LINUX_EFAULT;
    if (guest_read_str(g, name_gva, name, sizeof(name)) < 0)
        return -LINUX_EFAULT;

    char hostbuf[LINUX_PATH_MAX];
    const char *hpath = path;
    const hl_mount_t *mnt = NULL;
    /* follow = !nofollow. Resolving through the final symlink handed the
     * TARGET's host path to a call that then asked for XATTR_NOFOLLOW,
     * so every l*xattr variant behaved exactly like its non-l form. */
    int rr = resolve_path_for_op_ex(LINUX_AT_FDCWD, path, 0, !nofollow,
                                    hostbuf, sizeof(hostbuf), &hpath,
                                    &mnt);
    if (rr < 0) return rr;
    /* removexattr writes; a read-only or virtual mount must refuse it. The
     * mount was never even requested here, so every RO bind stayed
     * writable through extended attributes. */
    if (mnt && (mnt->flags & (HL_MOUNT_RO | HL_MOUNT_VIRTUAL)))
        return -LINUX_EROFS;

    int opts = nofollow ? XATTR_NOFOLLOW : 0;
    int ret = removexattr(hpath, name, opts);
    return ret < 0 ? linux_errno() : 0;
}

int64_t sys_fgetxattr(guest_t *g, int fd, uint64_t name_gva,
                      uint64_t value_gva, uint64_t size) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    char name[256];
    if (guest_read_str(g, name_gva, name, sizeof(name)) < 0)
        return -LINUX_EFAULT;

    if (size == 0) {
        ssize_t ret = fgetxattr(host_fd, name, NULL, 0, 0, 0);
        return ret < 0 ? linux_errno() : ret;
    }

    /* Bounce buffer: guest region may span a 2MB block boundary. */
    char *buf = malloc(size);
    if (!buf) return -LINUX_ENOMEM;

    ssize_t ret = fgetxattr(host_fd, name, buf, size, 0, 0);
    if (ret >= 0 && guest_write(g, value_gva, buf, ret) < 0)
        ret = -LINUX_EFAULT;
    else if (ret < 0)
        ret = linux_errno();
    free(buf);
    return ret;
}

int64_t sys_fsetxattr(guest_t *g, int fd, uint64_t name_gva,
                      uint64_t value_gva, uint64_t size, int flags) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    char name[256];
    if (guest_read_str(g, name_gva, name, sizeof(name)) < 0)
        return -LINUX_EFAULT;

    /* Bounce buffer: guest region may span a 2MB block boundary. */
    char *buf = NULL;
    if (size > 0) {
        buf = malloc(size);
        if (!buf) return -LINUX_ENOMEM;
        if (guest_read(g, value_gva, buf, size) < 0) { free(buf); return -LINUX_EFAULT; }
    }

    int opts = 0;
    if (flags & 1) opts |= XATTR_CREATE;
    if (flags & 2) opts |= XATTR_REPLACE;

    int ret = fsetxattr(host_fd, name, buf, size, 0, opts);
    int64_t result = ret < 0 ? linux_errno() : 0;
    free(buf);
    return result;
}

int64_t sys_flistxattr(guest_t *g, int fd, uint64_t list_gva, uint64_t size) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    if (size == 0) {
        ssize_t ret = flistxattr(host_fd, NULL, 0, 0);
        return ret < 0 ? linux_errno() : ret;
    }

    /* Bounce buffer: guest region may span a 2MB block boundary. */
    char *buf = malloc(size);
    if (!buf) return -LINUX_ENOMEM;

    ssize_t ret = flistxattr(host_fd, buf, size, 0);
    if (ret >= 0 && guest_write(g, list_gva, buf, ret) < 0)
        ret = -LINUX_EFAULT;
    else if (ret < 0)
        ret = linux_errno();
    free(buf);
    return ret;
}

int64_t sys_fremovexattr(guest_t *g, int fd, uint64_t name_gva) {
    int host_fd = fd_to_host(fd);
    if (host_fd < 0) return -LINUX_EBADF;

    char name[256];
    if (guest_read_str(g, name_gva, name, sizeof(name)) < 0)
        return -LINUX_EFAULT;

    int ret = fremovexattr(host_fd, name, 0);
    return ret < 0 ? linux_errno() : 0;
}
