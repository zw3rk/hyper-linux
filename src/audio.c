/* audio.c — Audio manager, stream, bounded transport, gain
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Core Audio is isolated in audio_coreaudio.c. This file has no AudioToolbox.
 */
#include "audio.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdatomic.h>
#include <time.h>

/* Default production backend; CLI/env may override before first stream. */
static hl_audio_backend_kind_t g_backend = HL_AUDIO_BACKEND_COREAUDIO;
static char g_wav_path[1024] = "hl-audio-debug.wav";
static atomic_ullong g_stream_id = 1;
static int g_audio_inited = 0;
/* Set when --audio-backend named a backend explicitly. HL_AUDIO_BACKEND must
 * not override an explicit flag: hl_audio_init() runs from syscall_init(),
 * i.e. AFTER option parsing, so the env read used to silently win. */
static int g_backend_explicit = 0;

void hl_audio_set_wav_path(const char *path) {
    if (path)
        snprintf(g_wav_path, sizeof(g_wav_path), "%s", path);
}

static const char *hl_audio_wav_path(void) {
    return g_wav_path;
}

void hl_audio_init(void) {
    if (g_audio_inited) return;
    g_audio_inited = 1;
    const char *env = getenv("HL_AUDIO_BACKEND");
    if (env && !g_backend_explicit) hl_audio_set_backend_name(env);
    env = getenv("HL_AUDIO_WAV");
    if (env) hl_audio_set_wav_path(env);
}

void hl_audio_shutdown(void) {
    g_audio_inited = 0;
}

void hl_audio_set_backend(hl_audio_backend_kind_t kind) {
    g_backend = kind;
}

/* Record that the backend came from the command line, so the environment
 * variable does not override it later. */
void hl_audio_set_backend_explicit(void) {
    g_backend_explicit = 1;
}

const char *hl_audio_backend_name(void) {
    switch (g_backend) {
    case HL_AUDIO_BACKEND_NULL:          return "null";
    case HL_AUDIO_BACKEND_NULL_REALTIME: return "null-realtime";
    case HL_AUDIO_BACKEND_WAV:           return "wav";
    case HL_AUDIO_BACKEND_COREAUDIO:     return "coreaudio";
    }
    return "coreaudio";
}

hl_audio_backend_kind_t hl_audio_get_backend(void) {
    return g_backend;
}

int hl_audio_set_backend_name(const char *name) {
    if (!name) return -1;
    if (strcmp(name, "null") == 0) { g_backend = HL_AUDIO_BACKEND_NULL; return 0; }
    if (strcmp(name, "null-realtime") == 0 || strcmp(name, "null_realtime") == 0) {
        g_backend = HL_AUDIO_BACKEND_NULL_REALTIME; return 0;
    }
    if (strcmp(name, "wav") == 0) { g_backend = HL_AUDIO_BACKEND_WAV; return 0; }
    if (strcmp(name, "coreaudio") == 0) { g_backend = HL_AUDIO_BACKEND_COREAUDIO; return 0; }
    return -1;
}

static const hl_audio_backend_ops_t *select_backend(hl_audio_backend_kind_t k) {
    switch (k) {
    case HL_AUDIO_BACKEND_NULL: return hl_audio_backend_null();
    case HL_AUDIO_BACKEND_NULL_REALTIME: return hl_audio_backend_null_realtime();
    case HL_AUDIO_BACKEND_WAV: return hl_audio_backend_wav();
    case HL_AUDIO_BACKEND_COREAUDIO: {
        const hl_audio_backend_ops_t *ops = hl_audio_backend_coreaudio();
        if (ops) return ops;
        return hl_audio_backend_null();
    }
    }
    return hl_audio_backend_null();
}

/* Worker: drain cons_fd for null/WAV backends only.
 * Core Audio owns cons_fd consumption in its AQ callback — never start
 * this worker for HL_AUDIO_BACKEND_COREAUDIO (double-read would steal PCM). */
static void *audio_worker(void *arg) {
    hl_audio_stream_t *s = arg;
    uint8_t buf[8192];
    while (!s->stop_worker) {
        if (s->backend_kind == HL_AUDIO_BACKEND_COREAUDIO) {
            /* Should not run; defensive sleep. */
            struct timespec ts = {0, 50 * 1000 * 1000};
            nanosleep(&ts, NULL);
            continue;
        }
        ssize_t n = read(s->cons_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct timespec ts = {0, 2 * 1000 * 1000}; /* 2ms */
                nanosleep(&ts, NULL);
                continue;
            }
            break;
        }
        if (n == 0) break;

        atomic_fetch_add(&s->submitted, (uint64_t)n);
        /* Null backend completes immediately; realtime sleeps; wav writes. */
        if (s->backend_kind == HL_AUDIO_BACKEND_NULL_REALTIME && s->params.rate > 0) {
            int bps = (s->params.format == HL_AUDIO_FMT_S16_LE) ? 2 : 1;
            int bytes_per_sec = s->params.rate * s->params.channels * bps;
            if (bytes_per_sec > 0) {
                long ns = (long)((n * 1000000000LL) / bytes_per_sec);
                struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
                nanosleep(&ts, NULL);
            }
        }
        /* WAV backend: write PCM to file */
        if (s->backend && s->backend_state && s->backend_kind == HL_AUDIO_BACKEND_WAV) {
            FILE *f = (FILE *)s->backend_state;
            fwrite(buf, 1, (size_t)n, f);
        }
        atomic_fetch_add(&s->completed, (uint64_t)n);
        pthread_mutex_lock(&s->lock);
        pthread_cond_broadcast(&s->space_cond);
        pthread_mutex_unlock(&s->lock);
    }
    return NULL;
}

hl_audio_stream_t *hl_audio_stream_create(void) {
    hl_audio_init();
    hl_audio_stream_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->id = atomic_fetch_add(&g_stream_id, 1);
    s->generation = 1;
    s->vol_left = s->vol_right = s->master = 100;
    s->params.format = HL_AUDIO_FMT_S16_LE;
    s->params.channels = 2;
    s->params.rate = 44100;
    /* Larger default ring: fewer guest pselect/write wakeups under HVF. */
    s->params.frag_size = 8192;
    s->params.frag_count = 16;
    s->capacity = s->params.frag_size * s->params.frag_count;
    s->prod_fd = s->cons_fd = -1;
    s->backend_kind = g_backend;
    s->backend = select_backend(g_backend);
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->space_cond, NULL);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        free(s);
        return NULL;
    }
    /* Enlarge kernel buffers toward capacity */
    int bufsz = s->capacity;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    s->prod_fd = sv[0];
    s->cons_fd = sv[1];
    fcntl(s->cons_fd, F_SETFL, O_NONBLOCK);

    if (s->backend && s->backend->create)
        s->backend->create(s);

    /* Core Audio callback consumes cons_fd; other backends use worker. */
    s->stop_worker = 0;
    s->worker_started = 0;
    if (s->backend_kind != HL_AUDIO_BACKEND_COREAUDIO) {
        if (pthread_create(&s->worker, NULL, audio_worker, s) == 0)
            s->worker_started = 1;
    }

    if (hl_trace_on(HL_TRACE_AUDIO))
        hl_trace(HL_TRACE_AUDIO, "stream create id=%llu backend=%s cap=%d",
                 (unsigned long long)s->id,
                 s->backend ? s->backend->name : "?", s->capacity);
    return s;
}

void hl_audio_stream_destroy(hl_audio_stream_t *s) {
    if (!s) return;
    s->stop_worker = 1;
    if (s->prod_fd >= 0) { close(s->prod_fd); s->prod_fd = -1; }
    if (s->worker_started) {
        pthread_join(s->worker, NULL);
        s->worker_started = 0;
    }
    /* Tear the backend down BEFORE closing cons_fd. For Core Audio there is
     * no worker to join, and the AQ callback reads cons_fd from a real-time
     * thread; AudioQueueStop/Dispose(true) are synchronous and wait for
     * in-flight callbacks. Closing cons_fd first leaves a window where a
     * callback reads a recycled fd number and steals another fd's data. */
    if (s->backend && s->backend->destroy)
        s->backend->destroy(s);
    if (s->cons_fd >= 0) { close(s->cons_fd); s->cons_fd = -1; }
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->space_cond);
    free(s);
}

int hl_audio_stream_configure(hl_audio_stream_t *s, const hl_audio_params_t *p) {
    if (!s || !p) return -1;
    pthread_mutex_lock(&s->lock);
    int fmt_changed = (s->params.format   != p->format ||
                       s->params.channels != p->channels ||
                       s->params.rate     != p->rate);
    s->params = *p;
    /* Clamp BOTH ends. Only the SNDCTL_DSP_SETFRAGMENT ioctl path bounded
     * frag_size from above; the fork-import path replayed the guest's raw
     * SETFRAGMENT word, so a shift of 30 gave frag_size = 1GB and
     * frag_size * frag_count overflowed this int — wrapping to 0 (every
     * write blocks forever) or negative (cast to uint64_t, ~1.8e19 free
     * bytes, space accounting disabled). 65536 * 128 = 8MB fits easily. */
    if (s->params.frag_size < 256) s->params.frag_size = 256;
    if (s->params.frag_size > 65536) s->params.frag_size = 65536;
    if (s->params.frag_count < 2) s->params.frag_count = 2;
    if (s->params.frag_count > 128) s->params.frag_count = 128;
    s->capacity = s->params.frag_size * s->params.frag_count;
    /* Resize the transport to match. SO_SNDBUF was set once at create from
     * the pre-configure capacity and never updated, so after SETFRAGMENT
     * poll() readiness (socket space) and write() acceptance (capacity -
     * pending) disagreed: a non-blocking OSS client could spin
     * poll-ready / write-EAGAIN. */
    if (s->prod_fd >= 0) {
        int bufsz = s->capacity;
        setsockopt(s->prod_fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    }
    if (s->cons_fd >= 0) {
        int bufsz = s->capacity;
        setsockopt(s->cons_fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    }
    s->configured = 1;
    /* Force a backend restart so the new ASBD takes effect. Without this a
     * SETFMT/SPEED/CHANNELS issued after the first write was silently
     * ignored and the new PCM played through the old format. */
    if (fmt_changed)
        s->running = 0;
    pthread_mutex_unlock(&s->lock);
    if (hl_trace_on(HL_TRACE_AUDIO))
        hl_trace(HL_TRACE_AUDIO,
                 "stream=%llu configure rate=%d ch=%d fmt=%d frag=%d x %d",
                 (unsigned long long)s->id, p->rate, p->channels, p->format,
                 p->frag_size, p->frag_count);
    return 0;
}

int hl_audio_stream_reset(hl_audio_stream_t *s) {
    if (!s) return -1;
    pthread_mutex_lock(&s->lock);
    s->generation++;
    /* Drain producer side by reading consumer */
    uint8_t sink[4096];
    fcntl(s->cons_fd, F_SETFL, O_NONBLOCK);
    while (read(s->cons_fd, sink, sizeof(sink)) > 0) { }
    atomic_store(&s->accepted, 0);
    atomic_store(&s->submitted, 0);
    atomic_store(&s->completed, 0);
    if (s->backend && s->backend->reset)
        s->backend->reset(s);
    /* Allow the next write to re-post. `running` was set once and never
     * cleared, so post() short-circuited forever: ca_reset() only empties
     * the Audio Queue, and buffers are re-enqueued ONLY from the callback,
     * which then never fires again. The guest's next write blocked in the
     * space wait permanently — i.e. any XMMS stop/seek/track-change. */
    s->running = 0;
    pthread_cond_broadcast(&s->space_cond);
    pthread_mutex_unlock(&s->lock);
    if (hl_trace_on(HL_TRACE_AUDIO))
        hl_trace(HL_TRACE_AUDIO, "stream=%llu reset gen=%llu",
                 (unsigned long long)s->id, (unsigned long long)s->generation);
    return 0;
}

int hl_audio_stream_drain(hl_audio_stream_t *s) {
    if (!s) return -1;
    if (!s->running || s->failed) return 0;
    /* SNDCTL_DSP_SYNC must block until everything queued has been played.
     * It previously returned immediately, so apps ending a track lost the
     * tail. Bounded so a stalled device cannot wedge the guest: at 4kHz
     * mono (the slowest supported rate) a full ring is well under 5s. */
    /* Bound = time to play a full ring at the negotiated rate, plus 50%
     * slack, capped at 60s. The previous fixed 5000 iterations (~7s) was
     * derived from arithmetic that was simply wrong: a 128KB ring at
     * 4kHz mono U8 is 32.8 SECONDS, so SYNC returned success having
     * dropped ~28s of audio. */
    int bps  = (s->params.format == HL_AUDIO_FMT_S16_LE) ? 2 : 1;
    int chan = s->params.channels ? s->params.channels : 2;
    int rate = s->params.rate ? s->params.rate : 44100;
    long bytes_per_sec = (long)rate * chan * bps;
    long need_ms = bytes_per_sec > 0
        ? ((long)s->capacity * 1000 / bytes_per_sec) * 3 / 2 + 100
        : 5000;
    if (need_ms < 1000)  need_ms = 1000;
    if (need_ms > 60000) need_ms = 60000;
    const int max_iters = (int)need_ms;   /* 1ms per iteration */
    for (int i = 0; i < max_iters; i++) {
        hl_audio_space_t sp;
        hl_audio_stream_get_space(s, &sp);
        if (sp.pending == 0) return 0;
        if (s->stop_worker || s->failed) return 0;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
        nanosleep(&ts, NULL);
    }
    /* Timed out with audio still queued: the device is stuck. Say so
     * instead of reporting a completed drain. */
    if (hl_trace_on(HL_TRACE_AUDIO))
        hl_trace(HL_TRACE_AUDIO, "stream=%llu drain TIMEOUT after %ldms",
                 (unsigned long long)s->id, need_ms);
    return -1;
}

int hl_audio_stream_post(hl_audio_stream_t *s) {
    if (!s) return -1;

    /* Serialize start against a concurrent start/reset on the same stream.
     * ca_setup_queue() does AudioQueueStop + AudioQueueDispose and rebuilds
     * st->queue with no locking of its own, and the old `if (!s->running)`
     * check-then-act sat outside any lock — two threads (a write() and a
     * SNDCTL_DSP_POST ioctl on the same fd) could both observe !running and
     * both dispose the same AudioQueueRef. */
    pthread_mutex_lock(&s->lock);
    if (s->failed) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    if (s->running) {
        /* Already playing. Re-running backend->start() here rebuilt the
         * queue and discarded everything already enqueued, so a guest
         * issuing SNDCTL_DSP_POST per buffer lost ~93ms of audio each
         * time and got silence re-primed in its place. */
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    int rc = 0;
    if (s->backend && s->backend->start)
        rc = s->backend->start(s);
    if (rc < 0) {
        /* Latch the failure. Nothing will ever drain cons_fd, so without
         * this every writer would block forever waiting for space that
         * can never be freed. Wake anyone already parked. */
        s->failed = 1;
        pthread_cond_broadcast(&s->space_cond);
    } else {
        s->running = 1;
    }
    pthread_mutex_unlock(&s->lock);
    return rc < 0 ? -1 : 0;
}

void hl_audio_apply_gain(const hl_audio_stream_t *s,
                         const void *src, void *dst, size_t n,
                         hl_audio_format_t fmt, int channels) {
    int ml = s->master * s->vol_left;
    int mr = s->master * s->vol_right;
    /* scale 0..10000 → Q15-ish /10000 */
    if (fmt == HL_AUDIO_FMT_U8) {
        const uint8_t *in = src;
        uint8_t *out = dst;
        for (size_t i = 0; i < n; i++) {
            int ch = (channels == 2) ? (int)(i & 1) : 0;
            int g = ch ? mr : ml;
            int v = (int)in[i] - 128;
            v = (v * g) / 10000;
            if (v < -128) v = -128;
            if (v > 127) v = 127;
            out[i] = (uint8_t)(v + 128);
        }
    } else {
        size_t samples = n / 2;
        const int16_t *in = src;
        int16_t *out = dst;
        for (size_t i = 0; i < samples; i++) {
            int ch = (channels == 2) ? (int)(i & 1) : 0;
            int g = ch ? mr : ml;
            int32_t v = ((int32_t)in[i] * g) / 10000;
            if (v < -32768) v = -32768;
            if (v > 32767) v = 32767;
            out[i] = (int16_t)v;
        }
    }
}

void hl_audio_stream_get_space(hl_audio_stream_t *s, hl_audio_space_t *sp) {
    memset(sp, 0, sizeof(*sp));
    if (!s) return;
    sp->capacity = (uint64_t)s->capacity;
    sp->accepted = atomic_load(&s->accepted);
    sp->submitted = atomic_load(&s->submitted);
    sp->completed = atomic_load(&s->completed);
    sp->pending = (sp->accepted >= sp->completed)
        ? (sp->accepted - sp->completed) : 0;
    sp->free_bytes = (sp->pending < sp->capacity)
        ? (sp->capacity - sp->pending) : 0;
}

int hl_audio_stream_poll_fd(hl_audio_stream_t *s) {
    return s ? s->prod_fd : -1;
}

int64_t hl_audio_stream_write(hl_audio_stream_t *s, const void *buf, size_t n) {
    if (!s || !buf) return -1;
    if (s->failed) return -1;

    /* OSS apps (xmms) often write PCM without SNDCTL_DSP_POST. Core Audio
     * only drains cons_fd from the AQ callback, which never runs until
     * AudioQueueStart. Without auto-start, accepted hits capacity and
     * writers block forever in the free_bytes wait below. */
    if (!s->running) {
        if (hl_audio_stream_post(s) < 0) {
            errno = EIO;
            return -1;
        }
    }

    uint8_t *tmp = NULL;
    const void *outp = buf;
    if (s->master != 100 || s->vol_left != 100 || s->vol_right != 100) {
        tmp = malloc(n);
        if (!tmp) return -1;
        hl_audio_apply_gain(s, buf, tmp, n, s->params.format, s->params.channels);
        outp = tmp;
    }

    /* Non-blocking producer: blocking write on a full socketpair deadlocks
     * if the CA callback stalls; space accounting already gates us. */
    int prod_flags = fcntl(s->prod_fd, F_GETFL);
    if (prod_flags >= 0 && !(prod_flags & O_NONBLOCK))
        fcntl(s->prod_fd, F_SETFL, prod_flags | O_NONBLOCK);

    size_t done = 0;
    while (done < n) {
        hl_audio_space_t sp;
        hl_audio_stream_get_space(s, &sp);
        if (sp.free_bytes == 0) {
            if (s->nonblock) {
                free(tmp);
                if (done > 0) return (int64_t)done;
                errno = EAGAIN;
                return -1;
            }
            pthread_mutex_lock(&s->lock);
            while (1) {
                uint64_t acc = atomic_load(&s->accepted);
                uint64_t cmp = atomic_load(&s->completed);
                uint64_t pending = (acc >= cmp) ? (acc - cmp) : 0;
                if (pending < (uint64_t)s->capacity || s->stop_worker ||
                    s->failed) break;
                /* Timed wait: CA callback cannot broadcast under lock */
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 20 * 1000 * 1000;
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&s->space_cond, &s->lock, &ts);
            }
            int dead = s->failed || s->stop_worker;
            pthread_mutex_unlock(&s->lock);
            /* Backend died or the stream is being torn down: space will never
             * free up, so returning to the top would spin forever. */
            if (dead) {
                free(tmp);
                if (done > 0) return (int64_t)done;
                errno = EIO;
                return -1;
            }
            continue;
        }
        size_t chunk = n - done;
        if (chunk > sp.free_bytes) chunk = (size_t)sp.free_bytes;
        if (chunk > 8192) chunk = 8192;

        ssize_t w = write(s->prod_fd, (const uint8_t *)outp + done, chunk);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                if (s->nonblock) {
                    free(tmp);
                    if (done > 0) return (int64_t)done;
                    return -1;
                }
                /* Kernel socket full while counters still show space —
                 * brief yield for CA callback to drain. */
                usleep(1000);
                continue;
            }
            free(tmp);
            return -1;
        }
        done += (size_t)w;
        atomic_fetch_add(&s->accepted, (uint64_t)w);
    }
    free(tmp);
    return (int64_t)done;
}

void hl_audio_stream_set_volume(hl_audio_stream_t *s, int left, int right) {
    if (!s) return;
    if (left < 0) left = 0;
    if (left > 100) left = 100;
    if (right < 0) right = 0;
    if (right > 100) right = 100;
    pthread_mutex_lock(&s->lock);
    s->vol_left = left;
    s->vol_right = right;
    pthread_mutex_unlock(&s->lock);
}

void hl_audio_stream_get_volume(hl_audio_stream_t *s, int *left, int *right) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    if (left) *left = s->vol_left;
    if (right) *right = s->vol_right;
    pthread_mutex_unlock(&s->lock);
}

/* ---- null backends ---- */
static int null_create(hl_audio_stream_t *s) { (void)s; return 0; }
static void null_destroy(hl_audio_stream_t *s) { (void)s; }
static int null_start(hl_audio_stream_t *s) { s->running = 1; return 0; }
static int null_reset(hl_audio_stream_t *s) { (void)s; return 0; }

static const hl_audio_backend_ops_t ops_null = {
    .name = "null",
    .create = null_create,
    .start = null_start,
    .reset = null_reset,
    .destroy = null_destroy,
};
static const hl_audio_backend_ops_t ops_null_rt = {
    .name = "null-realtime",
    .create = null_create,
    .start = null_start,
    .reset = null_reset,
    .destroy = null_destroy,
};

const hl_audio_backend_ops_t *hl_audio_backend_null(void) { return &ops_null; }
const hl_audio_backend_ops_t *hl_audio_backend_null_realtime(void) {
    return &ops_null_rt;
}

/* ---- WAV backend ---- */
struct wav_state {
    FILE *f;
    uint64_t data_bytes;
    int header_written;
};

static int wav_create(hl_audio_stream_t *s) {
    struct wav_state *st = calloc(1, sizeof(*st));
    if (!st) return -1;
    st->f = fopen(hl_audio_wav_path(), "wb");
    if (!st->f) { free(st); return -1; }
    /* defer header until configure known — write placeholder */
    uint8_t hdr[44] = {0};
    fwrite(hdr, 1, 44, st->f);
    st->header_written = 0;
    s->backend_state = st->f; /* worker uses FILE* */
    /* keep st on stream via embedding: store FILE* only; header fix on destroy */
    free(st);
    return 0;
}

static void wav_write_header(FILE *f, const hl_audio_params_t *p, uint64_t data_bytes) {
    int bps = (p->format == HL_AUDIO_FMT_S16_LE) ? 16 : 8;
    int block = p->channels * (bps / 8);
    int byte_rate = p->rate * block;
    uint32_t chunk = 36 + (uint32_t)data_bytes;
    rewind(f);
    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmt_sz = 16;
    uint16_t audio_fmt = 1, ch = (uint16_t)p->channels;
    uint32_t rate = (uint32_t)p->rate;
    uint16_t ba = (uint16_t)block, bp = (uint16_t)bps;
    fwrite(&fmt_sz, 4, 1, f);
    fwrite(&audio_fmt, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&ba, 2, 1, f);
    fwrite(&bp, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t db = (uint32_t)data_bytes;
    fwrite(&db, 4, 1, f);
}

static void wav_destroy(hl_audio_stream_t *s) {
    FILE *f = s->backend_state;
    if (!f) return;
    uint64_t data_bytes = atomic_load(&s->completed);
    wav_write_header(f, &s->params, data_bytes);
    fclose(f);
    s->backend_state = NULL;
}

static const hl_audio_backend_ops_t ops_wav = {
    .name = "wav",
    .create = wav_create,
    .start = null_start,
    .reset = null_reset,
    .destroy = wav_destroy,
};

const hl_audio_backend_ops_t *hl_audio_backend_wav(void) { return &ops_wav; }
