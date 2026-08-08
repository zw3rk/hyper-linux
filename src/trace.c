/* trace.c — Category-based tracing for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <time.h>

uint32_t hl_trace_mask = 0;
int hl_trace_redact_paths = 0;

static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;

struct cat_name {
    const char *name;
    uint32_t bit;
};

static const struct cat_name cats[] = {
    { "fs",    HL_TRACE_FS },
    { "fd",    HL_TRACE_FD },
    { "dev",   HL_TRACE_DEV },
    { "audio", HL_TRACE_AUDIO },
    { "proc",  HL_TRACE_PROC },
    { "fork",  HL_TRACE_FORK },
    { "sys",   HL_TRACE_SYS },
    { "all",   HL_TRACE_ALL },
    { NULL, 0 }
};

const char *hl_trace_category_names(void) {
    return "fs,fd,dev,audio,proc,fork,sys,all";
}

int hl_trace_parse(const char *spec, uint32_t *out_mask,
                   char *errbuf, size_t errbuf_sz) {
    if (!spec || !out_mask) {
        if (errbuf && errbuf_sz)
            snprintf(errbuf, errbuf_sz, "null trace spec");
        return -1;
    }
    uint32_t mask = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ", \t", &save); tok;
         tok = strtok_r(NULL, ", \t", &save)) {
        if (*tok == '\0') continue;
        int found = 0;
        for (int i = 0; cats[i].name; i++) {
            if (strcmp(tok, cats[i].name) == 0) {
                mask |= cats[i].bit;
                found = 1;
                break;
            }
        }
        if (!found) {
            if (errbuf && errbuf_sz)
                snprintf(errbuf, errbuf_sz,
                         "unknown trace category '%s' (valid: %s)",
                         tok, hl_trace_category_names());
            return -1;
        }
    }
    *out_mask = mask;
    return 0;
}

int hl_trace_init_from_env(void) {
    /* Read redaction BEFORE the HL_TRACE early-return. Categories can also
     * be enabled with --trace=..., and this used to bail out first — so
     * `HL_TRACE_REDACT=1 hl --trace=fs` traced with full host paths and the
     * documented redaction was simply unreachable that way. */
    const char *redact = getenv("HL_TRACE_REDACT");
    if (redact && (*redact == '1' || *redact == 'y' || *redact == 'Y'))
        hl_trace_redact_paths = 1;

    const char *env = getenv("HL_TRACE");
    if (!env || !*env) return 0;
    char err[256];
    uint32_t mask = 0;
    if (hl_trace_parse(env, &mask, err, sizeof(err)) < 0) {
        fprintf(stderr, "hl: HL_TRACE: %s\n", err);
        return -1;
    }
    hl_trace_mask |= mask;
    return 0;
}

int hl_trace_enable(const char *spec, char *errbuf, size_t errbuf_sz) {
    uint32_t mask = 0;
    if (hl_trace_parse(spec, &mask, errbuf, errbuf_sz) < 0)
        return -1;
    hl_trace_mask |= mask;
    return 0;
}

void hl_trace_escape(char *dst, size_t dstsz, const char *src) {
    if (!dst || dstsz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (*p >= 0x20 && *p != '\\' && *p != '"') {
            if (o + 1 >= dstsz) break;
            dst[o++] = (char)*p;
        } else if (*p == '\\' || *p == '"') {
            if (o + 2 >= dstsz) break;
            dst[o++] = '\\';
            dst[o++] = (char)*p;
        } else {
            if (o + 4 >= dstsz) break;
            o += (size_t)snprintf(dst + o, dstsz - o, "\\x%02x", *p);
            if (o >= dstsz) { o = dstsz - 1; break; }
        }
    }
    dst[o] = '\0';
}

void hl_trace_path(char *dst, size_t dstsz, const char *path) {
    if (!dst || dstsz == 0) return;
    if (!path) { dst[0] = '\0'; return; }
    if (!hl_trace_redact_paths) {
        hl_trace_escape(dst, dstsz, path);
        return;
    }
    const char *home = getenv("HOME");
    char tmp[4096];
    if (home && home[0] && strncmp(path, home, strlen(home)) == 0) {
        snprintf(tmp, sizeof(tmp), "~%s", path + strlen(home));
        hl_trace_escape(dst, dstsz, tmp);
    } else {
        hl_trace_escape(dst, dstsz, path);
    }
}

/* Redact every occurrence of $HOME (not just a prefix) and escape control
 * characters, into dst. The one redactor shared by trace lines and the crash
 * report — hl_trace_path() only handled a leading prefix, so a host path
 * appearing mid-token (a guest argv like --data-dir=/Users/me/x) leaked. */
void hl_trace_redact(char *dst, size_t dstsz, const char *src) {
    if (!dst || dstsz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    if (!hl_trace_redact_paths) { hl_trace_escape(dst, dstsz, src); return; }
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl == 0) { hl_trace_escape(dst, dstsz, src); return; }
    char red[4096];
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < sizeof(red); ) {
        if (strncmp(src + i, home, hl) == 0) {
            red[o++] = '~';
            i += hl;
        } else {
            red[o++] = src[i++];
        }
    }
    red[o] = '\0';
    hl_trace_escape(dst, dstsz, red);
}

void hl_tracev(uint32_t cat, const char *fmt, va_list ap) {
    if ((hl_trace_mask & cat) == 0) return;

    const char *cname = "?";
    if (cat & HL_TRACE_FS) cname = "fs";
    else if (cat & HL_TRACE_FD) cname = "fd";
    else if (cat & HL_TRACE_DEV) cname = "dev";
    else if (cat & HL_TRACE_AUDIO) cname = "audio";
    else if (cat & HL_TRACE_PROC) cname = "proc";
    else if (cat & HL_TRACE_FORK) cname = "fork";
    else if (cat & HL_TRACE_SYS) cname = "sys";

    char body[1024];
    vsnprintf(body, sizeof(body), fmt, ap);

    /* Sanitize the finished line rather than each call site. Trace strings
     * carry guest-controlled path bytes, which reached the operator's
     * terminal raw — control characters and ANSI/OSC escapes included.
     * Applying it here also makes HL_TRACE_REDACT effective: hl_trace_path()
     * and hl_trace_escape() existed but had no callers at all, so the
     * documented redaction silently did nothing. */
    char safe[sizeof(body) * 4];
    hl_trace_redact(safe, sizeof(safe), body);

    pthread_mutex_lock(&trace_lock);
    fprintf(stderr, "hl[pid=%d tid=%lu] %s %s\n",
            (int)getpid(),
            (unsigned long)(uintptr_t)pthread_self(),
            cname, safe);
    fflush(stderr);
    pthread_mutex_unlock(&trace_lock);
}

void hl_trace(uint32_t cat, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    hl_tracev(cat, fmt, ap);
    va_end(ap);
}
