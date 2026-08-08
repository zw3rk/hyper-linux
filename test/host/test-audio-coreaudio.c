/* test-audio-coreaudio.c — the real Core Audio backend, compiled and run
 *
 * Every other audio test uses the null backend, and audio_coreaudio.c is
 * built WITHOUT -DHL_HAVE_COREAUDIO everywhere else — so the entire file,
 * ca_callback included, compiled to a stub returning NULL and none of the
 * production audio path was covered by anything. This lane builds it for
 * real and drives the backend lifecycle.
 *
 * What it pins:
 *   - the file compiles and links against AudioToolbox (a syntax or ABI
 *     error in the callback path used to be invisible to the suite);
 *   - hl_audio_backend_coreaudio() returns real ops, not the NULL stub;
 *   - create → configure → post → write → drain → RESET → post again works,
 *     which is the restart fix from the same series, on the backend where
 *     it actually mattered (the null backend's worker drains regardless of
 *     `running`, which is exactly why it hid the bug);
 *   - a format change after the first write re-arms the queue.
 *
 * Playback is a fraction of a second of silence at low volume. If no output
 * device is available (headless CI), AudioQueueStart fails and the test
 * reports SKIP rather than a false failure — the compile-and-link coverage
 * still holds.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "../test-harness.h"
#include "audio.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    int passes = 0, fails = 0;
    printf("test-audio-coreaudio: real Core Audio backend\n");

    TEST("the Core Audio backend is compiled in, not the stub");
    if (hl_audio_backend_coreaudio() != NULL) PASS();
    else {
        FAIL("hl_audio_backend_coreaudio() is NULL — built without "
             "HL_HAVE_COREAUDIO, so none of this file is covered");
        SUMMARY("test-audio-coreaudio");
        return 1;
    }

    hl_audio_set_backend(HL_AUDIO_BACKEND_COREAUDIO);
    hl_audio_stream_t *s = hl_audio_stream_create();
    if (!s) { printf("FAIL: stream_create\n"); return 1; }

    hl_audio_params_t p = { .format = HL_AUDIO_FMT_S16_LE, .channels = 2,
                            .rate = 44100, .frag_size = 4096,
                            .frag_count = 8 };
    hl_audio_stream_configure(s, &p);

    /* Silence: S16 zero really is silence, so this is inaudible. */
    static int16_t pcm[4096];
    memset(pcm, 0, sizeof(pcm));

    TEST("post() starts a real Audio Queue");
    int started = (hl_audio_stream_post(s) == 0 && s->running);
    if (started) PASS();
    else {
        printf("  (skipped: no usable output device)\n");
        hl_audio_stream_destroy(s);
        SUMMARY("test-audio-coreaudio");
        return fails ? 1 : 0;
    }

    TEST("writes are accepted by the running queue");
    {
        int64_t w = hl_audio_stream_write(s, pcm, sizeof(pcm));
        if (w > 0) PASS();
        else FAILF("write returned %lld", (long long)w);
    }

    TEST("drain() completes without timing out");
    if (hl_audio_stream_drain(s) == 0) PASS();
    else FAIL("drain timed out on a real queue");

    /* The restart defect lived here: RESET left `running` set, post()
     * short-circuited forever, and since Core Audio buffers are re-enqueued
     * only from the callback the stream never played again. */
    TEST("RESET then post() restarts the queue");
    {
        hl_audio_stream_reset(s);
        if (s->running) {
            FAIL("running still set after reset");
        } else if (hl_audio_stream_post(s) != 0 || !s->running) {
            FAIL("post after reset did not restart the queue");
        } else {
            int64_t w = hl_audio_stream_write(s, pcm, sizeof(pcm));
            if (w > 0) PASS();
            else FAILF("write after restart returned %lld", (long long)w);
        }
    }

    TEST("a format change after the first write re-arms the queue");
    {
        hl_audio_params_t q = p;
        q.rate = 22050;
        q.channels = 1;
        hl_audio_stream_configure(s, &q);
        if (s->running) {
            FAIL("running still set — new PCM would play at the old rate");
        } else if (hl_audio_stream_post(s) != 0 || !s->running) {
            FAIL("could not restart at the new format");
        } else PASS();
    }

    TEST("destroy() tears the queue down cleanly");
    {
        hl_audio_stream_destroy(s);
        PASS();   /* a crash or hang here is the failure */
    }

    SUMMARY("test-audio-coreaudio");
    return fails ? 1 : 0;
}
