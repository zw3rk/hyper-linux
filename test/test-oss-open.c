/* test-oss-open.c — OSS Tier-1 configure + write + GETOSPACE
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>

/* Linux OSS ioctl numbers — must match src/linux_oss_abi.h */
#define AFMT_S16_LE 0x00000010
#define _IOC(dir,type,nr,size) \
    (((dir) << 30) | ((type) << 8) | (nr) | ((size) << 16))
#define _IO(type,nr)        _IOC(0U,(type),(nr),0)
#define _IOR(type,nr,t)     _IOC(2U,(type),(nr),sizeof(t))
#define _IOWR(type,nr,t)    _IOC(3U,(type),(nr),sizeof(t))

#define SNDCTL_DSP_RESET       _IO('P', 0)
#define SNDCTL_DSP_SPEED       _IOWR('P', 2, int)
#define SNDCTL_DSP_STEREO      _IOWR('P', 3, int)
#define SNDCTL_DSP_GETBLKSIZE  _IOWR('P', 4, int)
#define SNDCTL_DSP_SETFMT      _IOWR('P', 5, int)
#define SNDCTL_DSP_CHANNELS    _IOWR('P', 6, int)
#define SNDCTL_DSP_SETFRAGMENT _IOWR('P', 10, int)
#define SNDCTL_DSP_GETOSPACE   _IOR('P', 12, audio_buf_info)

typedef struct {
    int fragments;
    int fragstotal;
    int fragsize;
    int bytes;
} audio_buf_info;

int main(void) {
    int passes = 0, fails = 0;
    printf("test-oss-open: OSS Tier-1 path\n");

    int fd = open("/dev/dsp", O_WRONLY);
    TEST("open /dev/dsp");
    if (fd < 0) { FAIL("open"); SUMMARY("test-oss-open"); return 1; }
    PASS();

    int fmt = AFMT_S16_LE;
    TEST("SETFMT S16_LE");
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) == 0 && fmt == AFMT_S16_LE) PASS();
    else FAIL("SETFMT");

    int stereo = 1;
    TEST("STEREO");
    if (ioctl(fd, SNDCTL_DSP_STEREO, &stereo) == 0) PASS();
    else FAIL("STEREO");

    int ch = 2;
    TEST("CHANNELS");
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) == 0 && ch == 2) PASS();
    else FAIL("CHANNELS");

    int speed = 44100;
    TEST("SPEED 44100");
    if (ioctl(fd, SNDCTL_DSP_SPEED, &speed) == 0 && speed == 44100) PASS();
    else FAIL("SPEED");

    int frag = (16 << 16) | 12; /* 16 frags of 2^12=4096 */
    TEST("SETFRAGMENT");
    if (ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag) == 0) PASS();
    else FAIL("SETFRAGMENT");

    int blk = 0;
    TEST("GETBLKSIZE");
    if (ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &blk) == 0 && blk > 0) PASS();
    else FAIL("GETBLKSIZE");

    audio_buf_info info;
    memset(&info, 0, sizeof(info));
    TEST("GETOSPACE");
    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == 0 && info.bytes > 0) PASS();
    else FAIL("GETOSPACE");

    /* Write a short silence burst */
    int16_t silence[512];
    memset(silence, 0, sizeof(silence));
    TEST("write PCM");
    ssize_t w = write(fd, silence, sizeof(silence));
    if (w == (ssize_t)sizeof(silence)) PASS();
    else FAIL("write");

    TEST("RESET");
    if (ioctl(fd, SNDCTL_DSP_RESET) == 0) PASS();
    else FAIL("RESET");

    memset(&info, 0, sizeof(info));
    TEST("GETOSPACE after RESET");
    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == 0 && info.bytes > 0) PASS();
    else FAIL("GETOSPACE post-reset");

    close(fd);
    SUMMARY("test-oss-open");
    return fails ? 1 : 0;
}
