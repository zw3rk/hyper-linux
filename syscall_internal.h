/* syscall_internal.h — Shared helpers for syscall modules
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Declares fd_table and helper functions defined in syscall.c that are
 * needed by syscall_proc.c (and future syscall modules). These were
 * previously static; removing static lets them be shared across modules.
 */
#ifndef SYSCALL_INTERNAL_H
#define SYSCALL_INTERNAL_H

#include "syscall.h"

/* ---------- Named constants ---------- */

/* Linux PATH_MAX (4096) — used for path buffer sizing in syscall handlers.
 * Literal 4096 in guest.c/stack.c means actual page size, not this. */
#define LINUX_PATH_MAX 4096

/* ---------- FD table (owned by syscall.c) ---------- */
extern fd_entry_t fd_table[FD_TABLE_SIZE];

/* ---------- FD helpers ---------- */

/* Allocate the lowest available FD. Returns -1 if table is full. */
int fd_alloc(int type, int host_fd);

/* Allocate the lowest available FD >= minfd. Returns -1 if none available. */
int fd_alloc_from(int minfd, int type, int host_fd);

/* Allocate a specific FD slot. Returns -1 if out of range. */
int fd_alloc_at(int fd, int type, int host_fd);

/* Look up a guest FD. Returns host FD or -1 if invalid. */
int fd_to_host(int guest_fd);

/* ---------- Translation helpers ---------- */

/* Convert macOS errno to negative Linux errno. */
int64_t linux_errno(void);

/* Resolve dirfd: translate LINUX_AT_FDCWD and guest FDs. */
int resolve_dirfd(int dirfd);

/* Translate Linux AT_* flags to macOS equivalents. */
int translate_at_flags(int linux_flags);

/* Translate Linux open flags to macOS equivalents. */
int translate_open_flags(int linux_flags);

/* ---------- Guest memory helpers (from guest.h, used by net module) ---------- */
/* These are already declared in guest.h but listed here for reference:
 *   void *guest_ptr(guest_t *g, uint64_t gva)
 *   int guest_read(guest_t *g, uint64_t gva, void *buf, size_t len)
 *   int guest_write(guest_t *g, uint64_t gva, const void *buf, size_t len)
 */

#endif /* SYSCALL_INTERNAL_H */
