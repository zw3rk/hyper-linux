/* syscall_sys.h — System info and identity syscall handlers for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Uname, getrandom, getcwd, sched_getaffinity, getgroups, getrusage,
 * sysinfo, and prlimit64. System queries that don't fit neatly into
 * filesystem, I/O, or time categories.
 */
#ifndef SYSCALL_SYS_H
#define SYSCALL_SYS_H

#include <stdint.h>
#include "guest.h"

/* ---------- System info syscall handlers ---------- */

int64_t sys_uname(guest_t *g, uint64_t buf_gva);
int64_t sys_getrandom(guest_t *g, uint64_t buf_gva, uint64_t buflen,
                      unsigned int flags);
int64_t sys_getcwd(guest_t *g, uint64_t buf_gva, uint64_t size);
int64_t sys_sched_getaffinity(guest_t *g, int pid, uint64_t size,
                              uint64_t mask_gva);
int64_t sys_getgroups(guest_t *g, int size, uint64_t list_gva);
int64_t sys_getrusage(guest_t *g, int who, uint64_t usage_gva);
int64_t sys_sysinfo(guest_t *g, uint64_t info_gva);
int64_t sys_prlimit64(guest_t *g, int pid, int resource,
                      uint64_t new_gva, uint64_t old_gva);

#endif /* SYSCALL_SYS_H */
