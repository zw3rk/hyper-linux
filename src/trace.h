/* trace.h — Category-based tracing for hl (fs/fd/dev/audio/proc/fork)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Categories are off by default. Enable via
 * HL_TRACE=fs,fd,dev,audio,proc,fork,sys or --trace=fs,audio.
 * Category `sys` turns on syscall volume stats (HL_SYSCALL_STATS).
 * Unknown categories produce a clear error.
 */
#ifndef HL_TRACE_H
#define HL_TRACE_H

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Trace category bits */
#define HL_TRACE_FS     (1u << 0)
#define HL_TRACE_FD     (1u << 1)
#define HL_TRACE_DEV    (1u << 2)
#define HL_TRACE_AUDIO  (1u << 3)
#define HL_TRACE_PROC   (1u << 4)
#define HL_TRACE_FORK   (1u << 5)
#define HL_TRACE_SYS    (1u << 6)  /* enables syscall volume stats (see syscall_stats) */
#define HL_TRACE_ALL    0x7Fu

/* Global mask; 0 means tracing off. */
extern uint32_t hl_trace_mask;

/* Path redaction for user-reportable logs (replaces home prefixes with ~). */
extern int hl_trace_redact_paths;

/* Parse comma-separated categories into a mask.
 * Returns 0 on success, -1 on unknown category (error written to errbuf). */
int hl_trace_parse(const char *spec, uint32_t *out_mask,
                   char *errbuf, size_t errbuf_sz);

/* Apply mask from env HL_TRACE and/or CLI. Env applied first; CLI ORs in. */
int hl_trace_init_from_env(void);
int hl_trace_enable(const char *spec, char *errbuf, size_t errbuf_sz);

/* True if category bit is enabled. */
static inline int hl_trace_on(uint32_t cat) {
    return (hl_trace_mask & cat) != 0;
}

/* Escape control chars into dst (NUL-terminated, truncated). */
void hl_trace_escape(char *dst, size_t dstsz, const char *src);

/* Optionally redact absolute host-like paths. */
void hl_trace_path(char *dst, size_t dstsz, const char *path);

/* Emit a single structured line: hl[pid=.. tid=..] cat message... */
void hl_trace(uint32_t cat, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

void hl_tracev(uint32_t cat, const char *fmt, va_list ap)
    __attribute__((format(printf, 2, 0)));

/* Category name helpers for docs/errors */
const char *hl_trace_category_names(void);

#endif /* HL_TRACE_H */
