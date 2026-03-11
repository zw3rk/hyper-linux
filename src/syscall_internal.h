/* syscall_internal.h — Shared helpers for syscall modules
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Declares fd_table and helper functions defined in syscall.c that are
 * needed by syscall_proc.c (and future syscall modules). These were
 * previously static; removing static lets them be shared across modules.
 *
 * Lock ordering (acquire in ascending order to prevent deadlocks):
 *   1. mmap_lock    (syscall.c)     — mmap/brk allocators + page tables
 *   2. pt_lock      (guest.c)       — page table pool allocator
 *   3. fd_lock      (syscall.c)     — FD table (alloc/close/dup)
 *   4. sig_lock     (syscall_signal.c) — signal handlers/pending/blocked
 *   5. thread_lock  (thread.c)      — thread table
 *   5a. sfd_lock    (syscall_fd.c)  — special fd (never held with thread_lock)
 *   6. pid_lock     (syscall_proc.c) — process table / wait state
 *   7. futex bucket (futex.c)       — per-bucket, index-ordered if >1
 *   7. inotify_lock (syscall_inotify.c) — inotify watch table
 */
#ifndef SYSCALL_INTERNAL_H
#define SYSCALL_INTERNAL_H

#include "syscall.h"
#include <pthread.h>

/* ---------- Named constants ---------- */

/* Linux PATH_MAX (4096) — used for path buffer sizing in syscall handlers.
 * Literal 4096 in guest.c/stack.c means actual page size, not this. */
#define LINUX_PATH_MAX 4096

/* ---------- Locks (owned by syscall.c) ---------- */
extern pthread_mutex_t mmap_lock;   /* Lock order: 1 — mmap/brk + page tables */
extern pthread_mutex_t fd_lock;     /* Lock order: 3 — FD table */

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

/* Mark an FD slot as closed (set type = FD_CLOSED and update bitmap).
 * Does NOT close the host FD or free type-specific resources (DIR*,
 * epoll instance) — caller must do that first. */
void fd_mark_closed(int fd);

/* Same as fd_mark_closed but requires fd_lock to be already held.
 * Used by sys_execve CLOEXEC loop which holds fd_lock for the entire scan. */
void fd_mark_closed_unlocked(int fd);

/* Atomically snapshot an fd entry and mark it closed.  Returns 1 if the
 * slot was open (snapshot written to *out), 0 if already closed.  Prevents
 * the TOCTOU race where two concurrent close() calls both snapshot the
 * same open entry and double-close the host fd. */
int fd_snapshot_and_close(int fd, fd_entry_t *out);

/* ---------- Translation helpers ---------- */

/* Convert macOS errno to negative Linux errno. */
int64_t linux_errno(void);

/* Resolve dirfd: translate LINUX_AT_FDCWD and guest FDs. */
int resolve_dirfd(int dirfd);

/* Translate Linux AT_* flags to macOS equivalents.
 * For unlinkat, fstatat, linkat, fchmodat, fchownat, utimensat. */
int translate_at_flags(int linux_flags);

/* Translate Linux faccessat flags to macOS equivalents.
 * Separate from translate_at_flags because Linux AT_EACCESS (0x200) shares
 * the same numeric value as AT_REMOVEDIR — the meaning is context-dependent. */
int translate_faccessat_flags(int linux_flags);

/* Translate Linux open flags to macOS equivalents. */
int translate_open_flags(int linux_flags);

/* Translate macOS status flags (F_GETFL result) to Linux equivalents. */
int mac_to_linux_status_flags(int mac_flags);

/* Translate Linux status flags (F_SETFL arg) to macOS equivalents. */
int linux_to_mac_status_flags(int linux_flags);

/* ---------- Global verbose flag ---------- */
/* Set once by hl.c main() from --verbose. Used by detached threads
 * (rosettad handler) that don't have access to a guest_t. For code
 * running in syscall context, prefer g->verbose instead. */
extern int hl_verbose;

/* ---------- Guest memory helpers (from guest.h, used by net module) ---------- */
/* These are already declared in guest.h but listed here for reference:
 *   void *guest_ptr(guest_t *g, uint64_t gva)
 *   int guest_read(guest_t *g, uint64_t gva, void *buf, size_t len)
 *   int guest_write(guest_t *g, uint64_t gva, const void *buf, size_t len)
 */

#endif /* SYSCALL_INTERNAL_H */
