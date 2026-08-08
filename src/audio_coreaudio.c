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
    /* Silence fill byte for the current format. Unsigned 8-bit PCM is
     * centered on 0x80 — filling with 0x00 there is full-scale negative
     * DC, i.e. a loud click on every start and every underrun. Computed
     * at setup so the real-time callback only reads it. */
    uint8_t silence;
    /* Bytes per frame for the current format; used to keep submitted
     * buffers frame-aligned. */
    uint32_t bpf;
    /* Carry for a partial frame left over from the previous callback. */
    uint8_t  carry[8];
    uint32_t carry_len;
    /* Guest bytes contained in each in-flight buffer, so `completed` can be
     * advanced when a buffer finishes PLAYING rather than when it is filled.
     * Silence padding is excluded so it never inflates the counter. */
    uint32_t buf_bytes[AQ_BUFFERS];
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
        memset(b->mAudioData, st->silence, b->mAudioDataBytesCapacity);
        b->mAudioDataByteSize = b->mAudioDataBytesCapacity;
        AudioQueueEnqueueBuffer(q, b, 0, NULL);
        return;
    }
    /* This buffer has finished playing. Retire its guest bytes now, so
     * `completed` tracks what the device actually played rather than what
     * was handed to the queue — SNDCTL_DSP_GETODELAY previously
     * under-reported by the whole queue depth (~93ms). */
    int slot = -1;
    for (int i = 0; i < AQ_BUFFERS; i++)
        if (st->bufs[i] == b) { slot = i; break; }
    if (slot >= 0 && st->buf_bytes[slot]) {
        atomic_fetch_add(&s->completed, (uint64_t)st->buf_bytes[slot]);
        st->buf_bytes[slot] = 0;
    }

    int n = (int)b->mAudioDataBytesCapacity;
    uint8_t *dst = (uint8_t *)b->mAudioData;

    /* Re-emit any partial frame held back last time, so the frame phase is
     * preserved. Without this a single guest write that is not a whole
     * number of frames shifted every following sample by a byte for the
     * rest of the stream (L/R swap / broadband noise). */
    uint32_t pre = st->carry_len;
    if (pre) {
        memcpy(dst, st->carry, pre);
        st->carry_len = 0;
    }

    ssize_t got = read(s->cons_fd, dst + pre, (size_t)n - pre);
    if (got < 0) got = 0;
    uint32_t have = pre + (uint32_t)got;

    /* Submit only whole frames; stash the tail for the next callback. */
    uint32_t bpf = st->bpf ? st->bpf : 1;
    uint32_t keep = have % bpf;
    if (keep && keep <= sizeof(st->carry)) {
        memcpy(st->carry, dst + (have - keep), keep);
        st->carry_len = keep;
        have -= keep;
    }

    if (have < (uint32_t)n)
        memset(dst + have, st->silence, (size_t)n - have);
    b->mAudioDataByteSize = (UInt32)n;

    /* Counters: no lock (callback restriction). Worker is not started for CA. */
    if (got > 0)
        atomic_fetch_add(&s->submitted, (uint64_t)got);
    if (slot >= 0)
        st->buf_bytes[slot] = have;   /* retired when this buffer completes */
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

    /* S16 silence is 0x00; unsigned 8-bit PCM is centered on 0x80. */
    st->silence = (s->params.format == HL_AUDIO_FMT_S16_LE) ? 0x00 : 0x80;
    st->bpf = asbd.mBytesPerFrame ? asbd.mBytesPerFrame : 1;
    st->carry_len = 0;
    for (int i = 0; i < AQ_BUFFERS; i++) st->buf_bytes[i] = 0;

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
        memset(st->bufs[i]->mAudioData, st->silence, AQ_BUF_SIZE);
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
    if (!st) return -1;
    /* Discard the partial-frame carry and the per-buffer byte credits.
     * RESET must drop buffered audio: stale carry bytes were being
     * prepended to the next track (shifting every following sample), and
     * stale buf_bytes were added to a freshly-zeroed `completed`, pushing
     * it above `accepted` so GETOSPACE advertised space that did not exist.
     * Cleared before the queue check so a not-yet-started stream is reset
     * too. */
    st->carry_len = 0;
    for (int i = 0; i < AQ_BUFFERS; i++) st->buf_bytes[i] = 0;
    if (!st->queue) return -1;
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
