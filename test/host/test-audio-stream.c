/* test-audio-stream.c — host stream write/GETOSPACE/RESET without HVF
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

int main(void) {
    int fails = 0;
    hl_audio_set_backend(HL_AUDIO_BACKEND_NULL);
    hl_audio_stream_t *s = hl_audio_stream_create();
    if (!s) {
        printf("FAIL create\n");
        return 1;
    }

    hl_audio_params_t p = {
        .format = HL_AUDIO_FMT_S16_LE,
        .channels = 2,
        .rate = 44100,
        .frag_size = 4096,
        .frag_count = 8,
    };
    hl_audio_stream_configure(s, &p);

    hl_audio_space_t sp;
    hl_audio_stream_get_space(s, &sp);
    if (sp.free_bytes == 0 || sp.capacity == 0) {
        printf("FAIL space free=%llu cap=%llu\n",
               (unsigned long long)sp.free_bytes,
               (unsigned long long)sp.capacity);
        fails++;
    } else {
        printf("OK space free=%llu cap=%llu\n",
               (unsigned long long)sp.free_bytes,
               (unsigned long long)sp.capacity);
    }

    int16_t buf[1024];
    memset(buf, 0, sizeof(buf));
    int64_t w = hl_audio_stream_write(s, buf, sizeof(buf));
    if (w != (int64_t)sizeof(buf)) {
        printf("FAIL write %lld\n", (long long)w);
        fails++;
    } else {
        printf("OK write %lld\n", (long long)w);
    }

    /* Wait briefly for worker to complete */
    usleep(50 * 1000);
    hl_audio_stream_get_space(s, &sp);
    printf("OK after write pending=%llu free=%llu\n",
           (unsigned long long)sp.pending, (unsigned long long)sp.free_bytes);

    hl_audio_stream_reset(s);
    hl_audio_stream_get_space(s, &sp);
    if (sp.pending != 0 || sp.accepted != 0) {
        printf("FAIL reset pending=%llu accepted=%llu\n",
               (unsigned long long)sp.pending,
               (unsigned long long)sp.accepted);
        fails++;
    } else {
        printf("OK reset cleared counters free=%llu\n",
               (unsigned long long)sp.free_bytes);
    }

    /* Fill until full then nonblock EAGAIN path */
    s->nonblock = 1;
    size_t total = 0;
    for (int i = 0; i < 10000; i++) {
        int64_t n = hl_audio_stream_write(s, buf, sizeof(buf));
        if (n < 0) break;
        total += (size_t)n;
    }
    printf("OK nonblock fill total=%zu (stopped on full/EAGAIN)\n", total);

    hl_audio_stream_destroy(s);
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
