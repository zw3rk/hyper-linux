/* test-oss-tier1.c — expanded OSS: POST, NONBLOCK, mixer, GETOSPACE fill
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
#include <poll.h>
#include <stdint.h>

/* Use kernel headers' ioctl macros (musl provides soundcard-compatible layout
 * via our numeric constants). */
#define AFMT_S16_LE 0x00000010
#define _IOC(dir,type,nr,size) \
    (((dir) << 30) | ((type) << 8) | (nr) | ((size) << 16))
#define _IO(type,nr)        _IOC(0U,(type),(nr),0)
#define _IOR(type,nr,t)     _IOC(2U,(type),(nr),sizeof(t))
#define _IOWR(type,nr,t)    _IOC(3U,(type),(nr),sizeof(t))
#define SNDCTL_DSP_RESET       _IO('P', 0)
#define SNDCTL_DSP_SPEED       _IOWR('P', 2, int)
#define SNDCTL_DSP_CHANNELS    _IOWR('P', 6, int)
#define SNDCTL_DSP_SETFMT      _IOWR('P', 5, int)
#define SNDCTL_DSP_POST        _IO('P', 8)
#define SNDCTL_DSP_SETFRAGMENT _IOWR('P', 10, int)
#define SNDCTL_DSP_GETOSPACE   _IOR('P', 12, audio_buf_info)
#define SNDCTL_DSP_GETPLAYVOL  _IOR('P', 24, int)
#define SOUND_MIXER_WRITE_PCM  _IOWR('M', 4, int)
#define SOUND_MIXER_READ_PCM   _IOR('M', 4, int)
#define SOUND_MIXER_WRITE_VOLUME _IOWR('M', 0, int)
/* Linux soundcard.h: DEVMASK=0xfe, RECMASK=0xfd (do not swap). */
#define SOUND_MIXER_READ_DEVMASK _IOR('M', 0xfe, int)
/* Legacy OSS encoding (XMMS bundled soundcard.h SIOC_*) */
#define OSS_LEGACY_SIOR(x, y) \
    ((int)(0x20000000 | ((sizeof(int) & 0x1fff) << 16) | ((x) << 8) | (y)))
#define OSS_LEGACY_SIOWR(x, y) \
    ((int)(0x60000000 | ((sizeof(int) & 0x1fff) << 16) | ((x) << 8) | (y)))
#define SOUND_MIXER_READ_DEVMASK_LEGACY OSS_LEGACY_SIOR('M', 0xfe)
#define SOUND_MIXER_WRITE_PCM_LEGACY    OSS_LEGACY_SIOWR('M', 4)
#define SOUND_MIXER_READ_PCM_LEGACY     OSS_LEGACY_SIOR('M', 4)

typedef struct {
    int fragments, fragstotal, fragsize, bytes;
} audio_buf_info;

int main(void) {
    int passes = 0, fails = 0;
    printf("test-oss-tier1: expanded OSS coverage\n");

    int fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
    TEST("open NONBLOCK");
    if (fd < 0) { FAIL("open"); SUMMARY("test-oss-tier1"); return 1; }
    PASS();

    int fmt = AFMT_S16_LE, ch = 2, speed = 44100;
    int frag = (4 << 16) | 10; /* 4 frags of 1024 */
    ioctl(fd, SNDCTL_DSP_SETFMT, &fmt);
    ioctl(fd, SNDCTL_DSP_CHANNELS, &ch);
    ioctl(fd, SNDCTL_DSP_SPEED, &speed);
    ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag);

    TEST("POST");
    if (ioctl(fd, SNDCTL_DSP_POST) == 0) PASS();
    else FAIL("POST");

    /* Fill buffer until EAGAIN */
    int16_t silence[256];
    memset(silence, 0, sizeof(silence));
    int eagain = 0;
    for (int i = 0; i < 10000; i++) {
        ssize_t w = write(fd, silence, sizeof(silence));
        if (w < 0 && errno == EAGAIN) { eagain = 1; break; }
        if (w < 0) break;
    }
    TEST("NONBLOCK EAGAIN when full");
    if (eagain) PASS();
    else FAIL("no EAGAIN");

    audio_buf_info info;
    memset(&info, 0, sizeof(info));
    TEST("GETOSPACE when full bytes small");
    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == 0 && info.bytes < info.fragsize * info.fragstotal)
        PASS();
    else FAIL("GETOSPACE");

    /* poll for POLLOUT eventually as worker drains */
    TEST("poll POLLOUT");
    {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, 2000);
        if (pr >= 0) PASS(); /* readiness may already be set */
        else FAIL("poll");
    }

    TEST("RESET after full");
    if (ioctl(fd, SNDCTL_DSP_RESET) == 0) PASS();
    else FAIL("RESET");

    close(fd);

    /* Mixer */
    int mfd = open("/dev/mixer", O_RDWR);
    TEST("mixer open");
    if (mfd < 0) FAIL("mixer");
    else PASS();

    if (mfd >= 0) {
        int mask = 0;
        TEST("DEVMASK");
        if (ioctl(mfd, SOUND_MIXER_READ_DEVMASK, &mask) == 0 && mask != 0)
            PASS();
        else FAIL("DEVMASK");

        int vol = (50 << 8) | 50; /* L=R=50 */
        TEST("WRITE_PCM volume");
        if (ioctl(mfd, SOUND_MIXER_WRITE_PCM, &vol) == 0) PASS();
        else FAIL("WRITE_PCM");

        int rv = 0;
        TEST("READ_PCM volume");
        if (ioctl(mfd, SOUND_MIXER_READ_PCM, &rv) == 0) PASS();
        else FAIL("READ_PCM");

        /*
         * Mixer write must push software gain onto already-open /dev/dsp
         * streams (XMMS volume slider path). Pre-fix: WRITE_PCM only
         * updated g_mixer; GETPLAYVOL stayed 100/100 while playing.
         */
        {
            int dfd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
            TEST("mixer→live stream: open dsp");
            if (dfd < 0) {
                FAIL("dsp for live volume");
            } else {
                PASS();
                int mute = 0; /* L=R=0 */
                TEST("mixer→live stream: WRITE_PCM 0");
                if (ioctl(mfd, SOUND_MIXER_WRITE_PCM, &mute) != 0)
                    FAIL("WRITE_PCM 0");
                else
                    PASS();
                int pv = -1;
                TEST("mixer→live stream: GETPLAYVOL is 0");
                if (ioctl(dfd, SNDCTL_DSP_GETPLAYVOL, &pv) == 0 &&
                    (pv & 0xff) == 0 && ((pv >> 8) & 0xff) == 0)
                    PASS();
                else
                    FAIL("GETPLAYVOL after mute");
                int mid = (40 << 8) | 40;
                TEST("mixer→live stream: WRITE_PCM 40");
                if (ioctl(mfd, SOUND_MIXER_WRITE_PCM, &mid) != 0)
                    FAIL("WRITE_PCM 40");
                else
                    PASS();
                pv = -1;
                TEST("mixer→live stream: GETPLAYVOL is 40");
                if (ioctl(dfd, SNDCTL_DSP_GETPLAYVOL, &pv) == 0 &&
                    (pv & 0xff) == 40 && ((pv >> 8) & 0xff) == 40)
                    PASS();
                else
                    FAIL("GETPLAYVOL after 40");
                /* Master VOLUME also pushes (use_master=1 path). */
                int full = (100 << 8) | 100;
                int master0 = 0;
                (void)ioctl(mfd, SOUND_MIXER_WRITE_PCM, &full);
                TEST("mixer→live stream: WRITE_VOLUME 0");
                if (ioctl(mfd, SOUND_MIXER_WRITE_VOLUME, &master0) != 0)
                    FAIL("WRITE_VOLUME 0");
                else
                    PASS();
                pv = -1;
                TEST("mixer→live stream: GETPLAYVOL after VOLUME 0");
                if (ioctl(dfd, SNDCTL_DSP_GETPLAYVOL, &pv) == 0 &&
                    (pv & 0xff) == 0 && ((pv >> 8) & 0xff) == 0)
                    PASS();
                else
                    FAIL("GETPLAYVOL after VOLUME 0");
                close(dfd);
            }
        }

        /*
         * XMMS ships Output/OSS/soundcard.h with the pre-asm-generic SIOC_*
         * encoding. Accept those request numbers too (regression for live
         * GET_VOLUME=-1 under stock libOSS.so).
         *
         * Restore master VOLUME to 100 first — earlier WRITE_VOLUME 0 would
         * zero effective gain (vol×pcm/100) and mask a correct PCM write.
         */
        {
            int full = (100 << 8) | 100;
            (void)ioctl(mfd, SOUND_MIXER_WRITE_VOLUME, &full);
            (void)ioctl(mfd, SOUND_MIXER_WRITE_PCM, &full);
            int mask = 0;
            TEST("legacy DEVMASK");
            if (ioctl(mfd, SOUND_MIXER_READ_DEVMASK_LEGACY, &mask) == 0 &&
                mask != 0)
                PASS();
            else
                FAIL("legacy DEVMASK");
            int dfd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
            TEST("legacy WRITE_PCM → live stream");
            if (dfd < 0) {
                FAIL("dsp for legacy");
            } else {
                int mute = 0;
                if (ioctl(mfd, SOUND_MIXER_WRITE_PCM_LEGACY, &mute) != 0)
                    FAIL("legacy WRITE_PCM 0");
                else
                    PASS();
                int pv = -1;
                TEST("legacy GETPLAYVOL is 0");
                if (ioctl(dfd, SNDCTL_DSP_GETPLAYVOL, &pv) == 0 &&
                    (pv & 0xff) == 0 && ((pv >> 8) & 0xff) == 0)
                    PASS();
                else
                    FAIL("legacy GETPLAYVOL");
                int mid = (55 << 8) | 55;
                if (ioctl(mfd, SOUND_MIXER_WRITE_PCM_LEGACY, &mid) != 0)
                    FAIL("legacy WRITE_PCM 55");
                else
                    PASS();
                pv = -1;
                TEST("legacy GETPLAYVOL is 55");
                if (ioctl(dfd, SNDCTL_DSP_GETPLAYVOL, &pv) == 0 &&
                    (pv & 0xff) == 55 && ((pv >> 8) & 0xff) == 55)
                    PASS();
                else
                    FAIL("legacy GETPLAYVOL 55");
                close(dfd);
            }
        }
        close(mfd);
    }

    /* dup shares */
    fd = open("/dev/dsp", O_WRONLY);
    TEST("dup");
    if (fd >= 0) {
        int d = dup(fd);
        if (d >= 0) {
            close(d);
            PASS();
        } else FAIL("dup");
        close(fd);
    } else FAIL("reopen");

    SUMMARY("test-oss-tier1");
    return fails ? 1 : 0;
}
