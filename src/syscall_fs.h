/* syscall_fs.h — Filesystem syscall handlers for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stat, open, close, directory, xattr, permissions, and other filesystem
 * operations. Translates Linux aarch64 filesystem syscalls into macOS
 * equivalents, handling struct layout differences and flag translation.
 */
#ifndef SYSCALL_FS_H
#define SYSCALL_FS_H

#include <stdint.h>
#include "guest.h"

/* ---------- Filesystem syscall handlers ---------- */

/* stat/fstat family */
int64_t sys_fstat(guest_t *g, int fd, uint64_t stat_gva);
int64_t sys_newfstatat(guest_t *g, int dirfd, uint64_t path_gva,
                       uint64_t stat_gva, int flags);
int64_t sys_statfs(guest_t *g, uint64_t path_gva, uint64_t buf_gva);
int64_t sys_fstatfs(guest_t *g, int fd, uint64_t buf_gva);
int64_t sys_statx(guest_t *g, int dirfd, uint64_t path_gva,
                  int flags, unsigned int mask, uint64_t statxbuf_gva);

/* open/close/dup/fcntl */
int64_t sys_openat(guest_t *g, int dirfd, uint64_t path_gva,
                   int linux_flags, int mode);
int64_t sys_close(int fd);
int64_t sys_dup(int oldfd);
int64_t sys_dup3(int oldfd, int newfd, int linux_flags);
int64_t sys_fcntl(guest_t *g, int fd, int cmd, uint64_t arg);
int64_t sys_close_range(unsigned int first, unsigned int last,
                        unsigned int flags);

/* directory operations */
int64_t sys_getdents64(guest_t *g, int fd, uint64_t buf_gva, uint64_t count);
int64_t sys_chdir(guest_t *g, uint64_t path_gva);
int64_t sys_fchdir(int fd);
int64_t sys_chroot(guest_t *g, uint64_t path_gva);

/* pipe/seek */
int64_t sys_pipe2(guest_t *g, uint64_t fds_gva, int linux_flags);
int64_t sys_lseek(int fd, int64_t offset, int whence);

/* path operations */
int64_t sys_readlinkat(guest_t *g, int dirfd, uint64_t path_gva,
                       uint64_t buf_gva, uint64_t bufsiz);
int64_t sys_unlinkat(guest_t *g, int dirfd, uint64_t path_gva, int flags);
int64_t sys_mkdirat(guest_t *g, int dirfd, uint64_t path_gva, int mode);
int64_t sys_renameat2(guest_t *g, int olddirfd, uint64_t oldpath_gva,
                      int newdirfd, uint64_t newpath_gva, int flags);
int64_t sys_mknodat(guest_t *g, int dirfd, uint64_t path_gva,
                    int mode, int dev);
int64_t sys_symlinkat(guest_t *g, uint64_t target_gva,
                      int dirfd, uint64_t linkpath_gva);
int64_t sys_linkat(guest_t *g, int olddirfd, uint64_t oldpath_gva,
                   int newdirfd, uint64_t newpath_gva, int flags);
int64_t sys_faccessat(guest_t *g, int dirfd, uint64_t path_gva,
                      int mode, int flags);

/* truncate */
int64_t sys_ftruncate(int fd, int64_t length);
int64_t sys_truncate(guest_t *g, uint64_t path_gva, int64_t length);

/* permissions/ownership */
int64_t sys_fchmod(int fd, uint32_t mode);
int64_t sys_fchmodat(guest_t *g, int dirfd, uint64_t path_gva,
                     uint32_t mode, int flags);
int64_t sys_fchownat(guest_t *g, int dirfd, uint64_t path_gva,
                     uint32_t owner, uint32_t group, int flags);
int64_t sys_fchown(int fd, uint32_t owner, uint32_t group);
int64_t sys_utimensat(guest_t *g, int dirfd, uint64_t path_gva,
                      uint64_t times_gva, int flags);

/* xattr */
int64_t sys_getxattr(guest_t *g, uint64_t path_gva, uint64_t name_gva,
                     uint64_t value_gva, uint64_t size, int nofollow);
int64_t sys_setxattr(guest_t *g, uint64_t path_gva, uint64_t name_gva,
                     uint64_t value_gva, uint64_t size, int flags,
                     int nofollow);
int64_t sys_listxattr(guest_t *g, uint64_t path_gva,
                      uint64_t list_gva, uint64_t size, int nofollow);
int64_t sys_removexattr(guest_t *g, uint64_t path_gva,
                        uint64_t name_gva, int nofollow);
int64_t sys_fgetxattr(guest_t *g, int fd, uint64_t name_gva,
                      uint64_t value_gva, uint64_t size);
int64_t sys_fsetxattr(guest_t *g, int fd, uint64_t name_gva,
                      uint64_t value_gva, uint64_t size, int flags);
int64_t sys_flistxattr(guest_t *g, int fd, uint64_t list_gva, uint64_t size);
int64_t sys_fremovexattr(guest_t *g, int fd, uint64_t name_gva);

#endif /* SYSCALL_FS_H */
