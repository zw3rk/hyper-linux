/* test-oss-abi.c — ABI size/constant checks for linux_oss_abi.h
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "linux_oss_abi.h"
#include <stdio.h>
#include <stddef.h>

int main(void) {
    int fails = 0;
    if (sizeof(audio_buf_info) != 16) {
        printf("FAIL sizeof audio_buf_info=%zu\n", sizeof(audio_buf_info));
        fails++;
    } else printf("OK audio_buf_info size 16\n");

    if (AFMT_S16_LE != 0x10) { printf("FAIL AFMT_S16_LE\n"); fails++; }
    else printf("OK AFMT_S16_LE\n");
    if (AFMT_U8 != 0x08) { printf("FAIL AFMT_U8\n"); fails++; }
    else printf("OK AFMT_U8\n");

    /* ioctl numbers: type 'P', non-zero for SETFMT */
    if (SNDCTL_DSP_SETFMT == 0) { printf("FAIL SETFMT zero\n"); fails++; }
    else printf("OK SETFMT=0x%x\n", (unsigned)SNDCTL_DSP_SETFMT);
    if (SNDCTL_DSP_GETOSPACE == 0) { printf("FAIL GETOSPACE zero\n"); fails++; }
    else printf("OK GETOSPACE=0x%x\n", (unsigned)SNDCTL_DSP_GETOSPACE);

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
