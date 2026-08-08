/* test-audio-restart.c — the stream must be restartable after RESET
 *
 * Regression (-): hl_audio_stream_post() short-circuits on s->running, but
 * s->running was set once and NEVER cleared anywhere in the tree. After
 * SNDCTL_DSP_RESET the Core Audio queue is therefore never rebuilt —
 * ca_reset() only empties it, and buffers are re-enqueued solely from the
 * callback, which then never fires again. The guest's next write blocked in
 * the space wait forever: any XMMS stop / seek / track-change wedged.
 *
 * Same root cause silently ignored a SETFMT/SPEED/CHANNELS issued after the
 * first write, so new PCM played through the stale ASBD (and st->bpf /
 * st->silence stayed at the old format, disabling the frame-alignment and
 * U8-silence fixes on that path).
 *
 * (+) side: an unchanged configure(), and POST on an already-running stream,
 * must NOT force a restart — that was the original "POST discards queued
 * audio" bug and must stay fixed.
 *
 * This is a host unit test on purpose: the defect lives in backend-agnostic
 * logic in audio.c, so it needs no audio device. The guest suite runs
 * --audio-backend null, whose worker thread drains regardless of `running`,
 * which is exactly why nothing caught this.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../test-harness.h"
#include "audio.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    int passes = 0, fails = 0;
    printf("test-audio-restart: stream must restart after reset\n");

    hl_audio_set_backend(HL_AUDIO_BACKEND_NULL);
    hl_audio_stream_t *s = hl_audio_stream_create();
    if (!s) { printf("FAIL: stream_create\n"); return 1; }

    hl_audio_params_t p = { .format = HL_AUDIO_FMT_S16_LE, .channels = 2,
                            .rate = 44100, .frag_size = 4096,
                            .frag_count = 16 };
    hl_audio_stream_configure(s, &p);

    TEST("post() starts the stream");
    if (hl_audio_stream_post(s) == 0 && s->running) PASS();
    else FAILF("post rc/running = %d", s->running);

    /* (-) The bug: reset left running == 1 forever. */
    TEST("reset() clears running so the backend can restart");
    hl_audio_stream_reset(s);
    if (!s->running) PASS();
    else FAIL("running still set after reset — queue can never be rebuilt");

    TEST("post() after reset starts the stream again");
    if (hl_audio_stream_post(s) == 0 && s->running) PASS();
    else FAILF("post after reset rc/running = %d", s->running);

    /* (-) A format change after the first write was silently ignored. */
    TEST("configure() with a CHANGED format forces a restart");
    {
        hl_audio_params_t q = p;
        q.rate = 8000;
        q.channels = 1;
        q.format = HL_AUDIO_FMT_U8;
        hl_audio_stream_configure(s, &q);
        if (!s->running) PASS();
        else FAIL("running still set — new PCM would play at the old format");
    }

    /* (+) An unchanged configure must NOT restart: re-running backend start
     * is what discarded already-queued audio. */
    TEST("configure() with the SAME params does not restart");
    {
        hl_audio_stream_post(s);
        if (!s->running) { FAIL("precondition: not running"); }
        else {
            hl_audio_params_t q = s->params;   /* identical */
            hl_audio_stream_configure(s, &q);
            if (s->running) PASS();
            else FAIL("needless restart — this discards queued audio");
        }
    }

    /* (+) POST on an already-running stream stays a no-op. */
    TEST("post() on a running stream is a no-op");
    {
        uint64_t gen_before = s->generation;
        if (hl_audio_stream_post(s) == 0 && s->generation == gen_before) PASS();
        else FAIL("post rebuilt the queue on an already-running stream");
    }

    /* Drain must not spin forever on an idle stream. */
    TEST("drain() returns promptly with nothing queued");
    if (hl_audio_stream_drain(s) == 0) PASS();
    else FAIL("drain reported a timeout on an empty stream");

    hl_audio_stream_destroy(s);
    SUMMARY("test-audio-restart");
    return fails ? 1 : 0;
}
