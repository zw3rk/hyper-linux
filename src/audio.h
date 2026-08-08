/* audio.h — Audio manager, stream, transport, gain (no Core Audio here)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef HL_AUDIO_H
#define HL_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

typedef enum hl_audio_backend_kind {
    HL_AUDIO_BACKEND_NULL = 0,
    HL_AUDIO_BACKEND_NULL_REALTIME,
    HL_AUDIO_BACKEND_WAV,
    HL_AUDIO_BACKEND_COREAUDIO,
} hl_audio_backend_kind_t;

typedef enum hl_audio_format {
    HL_AUDIO_FMT_U8 = 0,
    HL_AUDIO_FMT_S16_LE,
} hl_audio_format_t;

typedef struct hl_audio_params {
    hl_audio_format_t format;
    int channels;     /* 1 or 2 */
    int rate;         /* Hz */
    int frag_size;    /* bytes */
    int frag_count;
} hl_audio_params_t;

typedef struct hl_audio_space {
    uint64_t capacity;
    uint64_t accepted;
    uint64_t submitted;
    uint64_t completed;
    uint64_t pending;
    uint64_t free_bytes;
} hl_audio_space_t;

typedef struct hl_audio_stream hl_audio_stream_t;

typedef struct hl_audio_backend_ops {
    const char *name;
    int (*create)(hl_audio_stream_t *s);
    int (*start)(hl_audio_stream_t *s);
    int (*pause)(hl_audio_stream_t *s);
    int (*resume)(hl_audio_stream_t *s);
    int (*reset)(hl_audio_stream_t *s);
    int (*drain)(hl_audio_stream_t *s);
    void (*destroy)(hl_audio_stream_t *s);
} hl_audio_backend_ops_t;

struct hl_audio_stream {
    uint64_t id;
    uint64_t generation;
    hl_audio_params_t params;
    hl_audio_backend_kind_t backend_kind;
    const hl_audio_backend_ops_t *backend;
    void *backend_state;

    int prod_fd;
    int cons_fd;
    int capacity;

    /* Syscall threads + CA callback (atomic; no lock in AQ callback). */
    _Atomic uint64_t accepted;
    _Atomic uint64_t submitted;
    _Atomic uint64_t completed;

    int vol_left;
    int vol_right;
    int master;

    int configured;
    int running;
    int nonblock;
    int failed;

    pthread_mutex_t lock;
    pthread_cond_t space_cond;
    pthread_t worker;
    int worker_started;
    int stop_worker;
};

void hl_audio_init(void);
void hl_audio_shutdown(void);

void hl_audio_set_backend(hl_audio_backend_kind_t kind);
hl_audio_backend_kind_t hl_audio_get_backend(void);
int hl_audio_set_backend_name(const char *name);
/* Mark the backend as explicitly chosen (CLI), so HL_AUDIO_BACKEND loses. */
void hl_audio_set_backend_explicit(void);
/* Current backend as a --audio-backend-compatible name. */
const char *hl_audio_backend_name(void);

hl_audio_stream_t *hl_audio_stream_create(void);
void hl_audio_stream_destroy(hl_audio_stream_t *s);

int hl_audio_stream_configure(hl_audio_stream_t *s, const hl_audio_params_t *p);
int hl_audio_stream_reset(hl_audio_stream_t *s);
int hl_audio_stream_post(hl_audio_stream_t *s);

int64_t hl_audio_stream_write(hl_audio_stream_t *s, const void *buf, size_t n);

void hl_audio_stream_get_space(hl_audio_stream_t *s, hl_audio_space_t *sp);
int hl_audio_stream_poll_fd(hl_audio_stream_t *s);

void hl_audio_stream_set_volume(hl_audio_stream_t *s, int left, int right);
void hl_audio_stream_get_volume(hl_audio_stream_t *s, int *left, int *right);

void hl_audio_apply_gain(const hl_audio_stream_t *s,
                         const void *src, void *dst, size_t n,
                         hl_audio_format_t fmt, int channels);

const hl_audio_backend_ops_t *hl_audio_backend_null(void);
const hl_audio_backend_ops_t *hl_audio_backend_null_realtime(void);
const hl_audio_backend_ops_t *hl_audio_backend_wav(void);
const hl_audio_backend_ops_t *hl_audio_backend_coreaudio(void);

void hl_audio_set_wav_path(const char *path);

#endif /* HL_AUDIO_H */
