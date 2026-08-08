/* test-oss-fork.c — OSS fork recreate-empty independent streams
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Parent opens /dev/dsp, configures, writes PCM, then forks.
 * Child must still have a usable dsp fd with empty free space
 * (recreate-empty; not parent's pending buffer).
 */
#include "test-harness.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdlib.h>

#define AFMT_S16_LE 0x00000010
/* Use musl ioctl.h macros if present; else encode. */
#ifndef _IOWR
#define _IOC(dir,type,nr,size) \
    (((dir) << 30) | ((type) << 8) | (nr) | ((size) << 16))
#define _IO(type,nr)        _IOC(0U,(type),(nr),0)
#define _IOR(type,nr,t)     _IOC(2U,(type),(nr),sizeof(t))
#define _IOWR(type,nr,t)    _IOC(3U,(type),(nr),sizeof(t))
#endif

#define SNDCTL_DSP_RESET       _IO('P', 0)
#define SNDCTL_DSP_SPEED       _IOWR('P', 2, int)
#define SNDCTL_DSP_CHANNELS    _IOWR('P', 6, int)
#define SNDCTL_DSP_SETFMT      _IOWR('P', 5, int)
#define SNDCTL_DSP_SETFRAGMENT _IOWR('P', 10, int)
#define SNDCTL_DSP_GETOSPACE   _IOR('P', 12, audio_buf_info)

typedef struct {
    int fragments, fragstotal, fragsize, bytes;
} audio_buf_info;

static int configure_dsp(int fd) {
    int fmt = AFMT_S16_LE, ch = 2, speed = 44100;
    int frag = (8 << 16) | 12;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) < 0) return -1;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) < 0) return -1;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &speed) < 0) return -1;
    if (ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag) < 0) return -1;
    return 0;
}

int main(void) {
    int passes = 0, fails = 0;
    printf("test-oss-fork: recreate-empty after fork\n");

    int fd = open("/dev/dsp", O_WRONLY);
    TEST("parent open /dev/dsp");
    if (fd < 0) { FAIL("open"); SUMMARY("test-oss-fork"); return 1; }
    PASS();

    TEST("parent configure");
    if (configure_dsp(fd) == 0) PASS();
    else FAIL("configure");

    int16_t silence[1024];
    memset(silence, 0, sizeof(silence));
    TEST("parent write");
    if (write(fd, silence, sizeof(silence)) == (ssize_t)sizeof(silence)) PASS();
    else FAIL("write");

    pid_t pid = fork();
    if (pid < 0) {
        TEST("fork");
        FAIL("fork");
        SUMMARY("test-oss-fork");
        return 1;
    }

    if (pid == 0) {
        int cfail = 0;
        setvbuf(stdout, NULL, _IONBF, 0);
        if (fcntl(fd, F_GETFD) < 0) {
            printf("  child fd still open            FAIL: EBADF\n");
            cfail++;
        } else {
            printf("  child fd still open            OK\n");
        }

        audio_buf_info info;
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) != 0) {
            printf("  child GETOSPACE empty-ish      FAIL\n");
            cfail++;
        } else {
            int cap = info.fragsize * (info.fragstotal > 0 ? info.fragstotal : 1);
            /* recreate-empty: free space should be near capacity */
            if (info.bytes >= cap / 2) {
                printf("  child GETOSPACE empty-ish      OK (bytes=%d cap~%d)\n",
                       info.bytes, cap);
            } else {
                printf("  child GETOSPACE empty-ish      FAIL: bytes=%d cap~%d\n",
                       info.bytes, cap);
                cfail++;
            }
        }

        if (ioctl(fd, SNDCTL_DSP_RESET) != 0) {
            printf("  child RESET                    FAIL\n");
            cfail++;
        } else {
            printf("  child RESET                    OK\n");
        }

        if (write(fd, silence, sizeof(silence)) != (ssize_t)sizeof(silence)) {
            printf("  child write independent        FAIL errno=%d\n", errno);
            cfail++;
        } else {
            printf("  child write independent        OK\n");
        }

        if (configure_dsp(fd) != 0) {
            printf("  child reconfigure              FAIL\n");
            cfail++;
        } else {
            printf("  child reconfigure              OK\n");
        }

        close(fd);
        _exit(cfail ? 1 : 0);
    }

    TEST("fork");
    PASS();

    int status = 0;
    TEST("wait child success");
    if (waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0)
        PASS();
    else {
        printf("FAIL: child status=0x%x exit=%d\n", status,
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        fails++;
    }

    TEST("parent write after fork");
    if (write(fd, silence, sizeof(silence)) == (ssize_t)sizeof(silence))
        PASS();
    else FAIL("parent write");

    TEST("parent GETOSPACE after fork");
    {
        audio_buf_info info;
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == 0) PASS();
        else FAIL("GETOSPACE");
    }

    close(fd);
    SUMMARY("test-oss-fork");
    return fails ? 1 : 0;
}
