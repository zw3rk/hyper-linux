/* syscall_inotify.h — Linux inotify emulation via kqueue EVFILT_VNODE for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulates Linux inotify using macOS kqueue EVFILT_VNODE as the backend.
 * Each inotify_add_watch opens the target path and registers a kevent.
 * Events are translated from kqueue NOTE_* flags to Linux IN_* flags and
 * queued internally. A self-pipe provides poll/epoll readability notification
 * (same pattern as eventfd/signalfd).
 */
#ifndef SYSCALL_INOTIFY_H
#define SYSCALL_INOTIFY_H

#include <stdint.h>
#include "guest.h"

/* Create an inotify instance. flags may include IN_NONBLOCK (0x800) and
 * IN_CLOEXEC (0x80000). Returns a guest fd or negative Linux errno. */
int64_t sys_inotify_init1(int flags);

/* Add or modify a watch on path_gva for events matching mask.
 * Returns a watch descriptor (>= 1) or negative Linux errno. */
int64_t sys_inotify_add_watch(guest_t *g, int inotify_fd,
                               uint64_t path_gva, uint32_t mask);

/* Remove an existing watch. Returns 0 or negative Linux errno. */
int64_t sys_inotify_rm_watch(int inotify_fd, int wd);

/* Read pending inotify events into the guest buffer. Called from
 * sys_read() when the FD type is FD_INOTIFY. Returns bytes read,
 * or negative Linux errno. */
int64_t inotify_read(int guest_fd, guest_t *g, uint64_t buf_gva,
                      uint64_t count);

/* Clean up inotify state when the guest closes the fd. Called from
 * sys_close(). */
void inotify_close(int guest_fd);

#endif /* SYSCALL_INOTIFY_H */
