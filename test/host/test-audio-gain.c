/* test-audio-gain.c — host unit test for software gain (no HVF)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compile: clang -I../../src -o test-audio-gain test-audio-gain.c ../../src/audio.c -lpthread
 * (or via make test-host-audio-gain)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Minimal stubs if linking only gain — include audio.h and link audio.o */
#include "audio.h"

int main(void) {
    int fails = 0;
    hl_audio_stream_t s;
    memset(&s, 0, sizeof(s));
    s.master = 50;
    s.vol_left = 100;
    s.vol_right = 100;

    int16_t in[4] = { 10000, -10000, 20000, -20000 };
    int16_t out[4];
    hl_audio_apply_gain(&s, in, out, sizeof(in), HL_AUDIO_FMT_S16_LE, 2);

    /* master 50% → ~half amplitude */
    if (out[0] < 4000 || out[0] > 6000) {
        printf("FAIL left gain: %d\n", out[0]);
        fails++;
    } else {
        printf("OK left gain %d\n", out[0]);
    }

    s.vol_left = 0;
    s.vol_right = 0;
    hl_audio_apply_gain(&s, in, out, sizeof(in), HL_AUDIO_FMT_S16_LE, 2);
    if (out[0] != 0 || out[1] != 0) {
        printf("FAIL mute\n");
        fails++;
    } else {
        printf("OK mute\n");
    }

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
