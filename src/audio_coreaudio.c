/* audio_coreaudio.c — Audio Queue Services production backend
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linked only when HL_HAVE_COREAUDIO is defined. Callback is constant-time:
 * no malloc, no guest memory, no logging, no contended locks.
 */
#include "audio.h"
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#if defined(HL_HAVE_COREAUDIO)
#include <AudioToolbox/AudioToolbox.h>
#include <AudioToolbox/AudioQueue.h>

#define AQ_BUFFERS 4
#define AQ_BUF_SIZE 4096

typedef struct ca_state {
    AudioQueueRef queue;
    AudioQueueBufferRef bufs[AQ_BUFFERS];
    hl_audio_stream_t *stream;
    uint64_t generation;
    int running;
} ca_state_t;

/* Callback: only copy from stream's cons_fd via pre-known state; no log/malloc. */
static void ca_callback(void *user, AudioQueueRef q, AudioQueueBufferRef b) {
    ca_state_t *st = user;
    if (!st || !st->stream) {
        b->mAudioDataByteSize = 0;
        AudioQueueEnqueueBuffer(q, b, 0, NULL);
        return;
    }
    /* generation check without lock: tear-down sets generation mismatch */
    hl_audio_stream_t *s = st->stream;
    if (st->generation != s->generation || s->stop_worker) {
        memset(b->mAudioData, 0, b->mAudioDataBytesCapacity);
        b->mAudioDataByteSize = b->mAudioDataBytesCapacity;
        AudioQueueEnqueueBuffer(q, b, 0, NULL);
        return;
    }
    int n = (int)b->mAudioDataBytesCapacity;
    ssize_t got = read(s->cons_fd, b->mAudioData, (size_t)n);
    if (got < 0) got = 0;
    if (got < n)
        memset((uint8_t *)b->mAudioData + got, 0, (size_t)(n - got));
    b->mAudioDataByteSize = (UInt32)n;
    /* Counters: no lock (callback restriction). Worker is not started for CA. */
    if (got > 0) {
        atomic_fetch_add(&s->completed, (uint64_t)got);
        atomic_fetch_add(&s->submitted, (uint64_t)got);
    }
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

/* Build / rebuild AQ for current s->params. Safe to call from create or
 * start: OSS apps configure format after open, before first write. */
static int ca_setup_queue(hl_audio_stream_t *s, ca_state_t *st) {
    if (st->queue) {
        AudioQueueStop(st->queue, true);
        AudioQueueDispose(st->queue, true);
        st->queue = NULL;
        memset(st->bufs, 0, sizeof(st->bufs));
    }

    AudioStreamBasicDescription asbd;
    memset(&asbd, 0, sizeof(asbd));
    int bps = (s->params.format == HL_AUDIO_FMT_S16_LE) ? 16 : 8;
    int ch = s->params.channels ? s->params.channels : 2;
    asbd.mSampleRate = s->params.rate ? s->params.rate : 44100;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsPacked;
    if (s->params.format == HL_AUDIO_FMT_S16_LE)
        asbd.mFormatFlags |= kAudioFormatFlagIsSignedInteger;
    asbd.mBytesPerPacket = (UInt32)(ch * (bps / 8));
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = asbd.mBytesPerPacket;
    asbd.mChannelsPerFrame = (UInt32)ch;
    asbd.mBitsPerChannel = (UInt32)bps;

    OSStatus err = AudioQueueNewOutput(&asbd, ca_callback, st, NULL, NULL, 0,
                                       &st->queue);
    if (err != noErr) {
        st->queue = NULL;
        return -1;
    }
    for (int i = 0; i < AQ_BUFFERS; i++) {
        err = AudioQueueAllocateBuffer(st->queue, AQ_BUF_SIZE, &st->bufs[i]);
        if (err != noErr) {
            AudioQueueDispose(st->queue, true);
            st->queue = NULL;
            return -1;
        }
        memset(st->bufs[i]->mAudioData, 0, AQ_BUF_SIZE);
        st->bufs[i]->mAudioDataByteSize = AQ_BUF_SIZE;
        AudioQueueEnqueueBuffer(st->queue, st->bufs[i], 0, NULL);
    }
    st->generation = s->generation;
    return 0;
}

static int ca_create(hl_audio_stream_t *s) {
    ca_state_t *st = calloc(1, sizeof(*st));
    if (!st) return -1;
    st->stream = s;
    st->generation = s->generation;
    s->backend_state = st;
    /* Defer real AQ setup to start/post so OSS SETFMT/SPEED apply first.
     * Still allocate state so write→post has somewhere to attach. */
    return 0;
}

static int ca_start(hl_audio_stream_t *s) {
    ca_state_t *st = s->backend_state;
    if (!st) return -1;
    /* (Re)build queue with latest params every start — cheap vs silence. */
    if (ca_setup_queue(s, st) < 0)
        return -1;
    OSStatus err = AudioQueueStart(st->queue, NULL);
    st->running = (err == noErr);
    if (hl_trace_on(HL_TRACE_AUDIO))
        hl_trace(HL_TRACE_AUDIO,
                 "coreaudio start rate=%d ch=%d fmt=%d err=%d",
                 s->params.rate, s->params.channels, s->params.format,
                 (int)err);
    return st->running ? 0 : -1;
}

static int ca_pause(hl_audio_stream_t *s) {
    ca_state_t *st = s->backend_state;
    if (!st || !st->queue) return -1;
    AudioQueuePause(st->queue);
    return 0;
}

static int ca_resume(hl_audio_stream_t *s) {
    return ca_start(s);
}

static int ca_reset(hl_audio_stream_t *s) {
    ca_state_t *st = s->backend_state;
    if (!st || !st->queue) return -1;
    st->generation = s->generation;
    AudioQueueReset(st->queue);
    return 0;
}

static void ca_destroy(hl_audio_stream_t *s) {
    ca_state_t *st = s->backend_state;
    if (!st) return;
    st->generation = 0;
    if (st->queue) {
        AudioQueueStop(st->queue, true);
        AudioQueueDispose(st->queue, true);
        st->queue = NULL;
    }
    free(st);
    s->backend_state = NULL;
}

static const hl_audio_backend_ops_t ops_ca = {
    .name = "coreaudio",
    .create = ca_create,
    .start = ca_start,
    .pause = ca_pause,
    .resume = ca_resume,
    .reset = ca_reset,
    .destroy = ca_destroy,
};

const hl_audio_backend_ops_t *hl_audio_backend_coreaudio(void) {
    return &ops_ca;
}

#else /* !HL_HAVE_COREAUDIO */

const hl_audio_backend_ops_t *hl_audio_backend_coreaudio(void) {
    return NULL; /* callers fall back to null */
}

#endif
