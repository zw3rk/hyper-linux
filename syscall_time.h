/* syscall_time.h — Time and timer syscall handlers for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clock, nanosleep, gettimeofday, and interval timer operations. Translates
 * Linux clock IDs to macOS equivalents and emulates ITIMER_REAL internally.
 */
#ifndef SYSCALL_TIME_H
#define SYSCALL_TIME_H

#include <stdint.h>
#include "guest.h"

/* ---------- Time/timer syscall handlers ---------- */

int64_t sys_clock_gettime(guest_t *g, int clockid, uint64_t tp_gva);
int64_t sys_nanosleep(guest_t *g, uint64_t req_gva, uint64_t rem_gva);
int64_t sys_clock_nanosleep(guest_t *g, int clockid, int flags,
                            uint64_t req_gva, uint64_t rem_gva);
int64_t sys_gettimeofday(guest_t *g, uint64_t tv_gva, uint64_t tz_gva);
int64_t sys_setitimer(guest_t *g, int which,
                      uint64_t new_gva, uint64_t old_gva);
int64_t sys_getitimer(guest_t *g, int which, uint64_t val_gva);

#endif /* SYSCALL_TIME_H */
