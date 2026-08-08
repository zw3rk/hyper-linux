/* syscall_stats.h — Lightweight guest-syscall volume profiling for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Enable with HL_SYSCALL_STATS=1 or HL_TRACE=sys.
 * Dump anytime with SIGUSR1 (or atexit / exit_group).
 */
#ifndef SYSCALL_STATS_H
#define SYSCALL_STATS_H

#include <stdint.h>
#include <stdio.h>

/* Max aarch64 syscall number we track (covers current Linux + margin). */
#define HL_SYS_STATS_MAX_NR 512

/* Init from env (HL_SYSCALL_STATS / HL_TRACE includes sys). Safe to call once. */
void syscall_stats_init(void);

/* True when counting is active. */
int syscall_stats_enabled(void);

/* Record one completed syscall (result already known). */
void syscall_stats_note(unsigned nr, int64_t result,
                        uint64_t a0, uint64_t a1, uint64_t a2);

/* Mark a guest FD as the X11 display connection (after connect to X11-unix). */
void syscall_stats_mark_x11_fd(int guest_fd);

/* Clear a guest FD tag on close (best-effort). */
void syscall_stats_clear_fd(int guest_fd);

/* Dump histogram + I/O totals to stderr (or fp). label is optional. */
void syscall_stats_dump(const char *label);
void syscall_stats_dump_fp(FILE *fp, const char *label);

/* Zero counters (keeps X11 fd tags). */
void syscall_stats_reset(void);

/* Install SIGUSR1 → dump (and optional reset if HL_SYSCALL_STATS_RESET=1). */
void syscall_stats_install_signal(void);

/* Register atexit dump. */
void syscall_stats_atexit_register(void);

/* Optional per-handler timing (HL_SYSCALL_STATS=time). */
int syscall_stats_time_handlers(void);
void syscall_stats_note_time_ns(unsigned long long ns);

#endif /* SYSCALL_STATS_H */
