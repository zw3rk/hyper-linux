/* syscall_fd.h — Special FD types (eventfd, signalfd, timerfd) for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulates Linux eventfd (pipe+counter), signalfd (synthetic signal reads),
 * and timerfd (kqueue EVFILT_TIMER). Each provides special read/write/close
 * semantics dispatched from sys_read/sys_write/sys_close.
 */
#ifndef SYSCALL_FD_H
#define SYSCALL_FD_H

#include <stdint.h>
#include "guest.h"

/* timerfd (emulated via kqueue) */
int64_t sys_timerfd_create(int clockid, int flags);
int64_t sys_timerfd_settime(guest_t *g, int fd, int flags,
                             uint64_t new_value_gva,
                             uint64_t old_value_gva);
int64_t sys_timerfd_gettime(guest_t *g, int fd, uint64_t curr_value_gva);

/* eventfd (emulated via pipe + counter) */
int64_t sys_eventfd2(unsigned int initval, int flags);

/* signalfd (emulated via synthetic signal reads) */
int64_t sys_signalfd4(guest_t *g, int fd, uint64_t mask_gva,
                       uint64_t sigsetsize, int flags);

/* Special read/write handlers for eventfd, signalfd, and timerfd FD types.
 * Called from sys_read/sys_write when the fd type requires special
 * semantics (8-byte counter for eventfd, signalfd_siginfo for signalfd,
 * 8-byte expiration count for timerfd). */
int64_t eventfd_read(int guest_fd, guest_t *g, uint64_t buf_gva, uint64_t count);
int64_t eventfd_write(int guest_fd, guest_t *g, uint64_t buf_gva, uint64_t count);
int64_t signalfd_read(int guest_fd, guest_t *g, uint64_t buf_gva, uint64_t count);
int64_t timerfd_read(int guest_fd, guest_t *g, uint64_t buf_gva, uint64_t count);

/* Cleanup functions called from sys_close() when closing special FD types.
 * Release internal state (pipe write-ends, kqueue fds, state slots). */
void eventfd_close(int guest_fd);
void signalfd_close(int guest_fd);
void timerfd_close(int guest_fd);

/* Notify signalfd pipes when a signal is queued. Called from
 * signal_queue() — writes a byte to make poll/epoll see readability. */
void signalfd_notify(int signum);

#endif /* SYSCALL_FD_H */
