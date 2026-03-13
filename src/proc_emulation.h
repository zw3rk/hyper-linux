/* proc_emulation.h — /proc and /dev path emulation for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Intercepts openat and readlinkat for /proc, /dev, and other synthetic
 * paths. Returns host fds for synthetic content or -2 if not intercepted.
 */
#ifndef PROC_EMULATION_H
#define PROC_EMULATION_H

#include <stddef.h>
#include <sys/stat.h>
#include "guest.h"

/* Intercept openat for /proc and /dev paths.
 * The guest_t pointer is needed to generate /proc/self/maps from region data.
 * Returns a host fd on match (caller should fd_alloc it), -1 on error
 * with errno set, or -2 if the path is not intercepted (fall through). */
int proc_intercept_open(const guest_t *g, const char *path, int linux_flags, int mode);

/* Intercept readlinkat for /proc paths.
 * Returns the link length on match, -1 on error, or -2 if not
 * intercepted (fall through to real readlinkat). */
int proc_intercept_readlink(const char *path, char *buf, size_t bufsiz);

/* Intercept stat/fstatat for /proc paths.
 * Returns 0 if stat was synthesized (mac_st filled), or -2 if not
 * intercepted (fall through to real fstatat). */
int proc_intercept_stat(const char *path, struct stat *mac_st);

#endif /* PROC_EMULATION_H */
