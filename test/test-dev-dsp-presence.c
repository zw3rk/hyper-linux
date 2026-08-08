/* test-dev-dsp-presence.c — /dev/dsp and /dev/mixer must exist as char devices
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>

int main(void) {
    int passes = 0, fails = 0;
    printf("test-dev-dsp-presence: virtual OSS device nodes\n");

    TEST("/dev/dsp open O_WRONLY");
    {
        int fd = open("/dev/dsp", O_WRONLY);
        if (fd >= 0) { close(fd); PASS(); }
        else FAIL("open /dev/dsp");
    }

    TEST("/dev/dsp0 open O_WRONLY");
    {
        int fd = open("/dev/dsp0", O_WRONLY);
        if (fd >= 0) { close(fd); PASS(); }
        else FAIL("open /dev/dsp0");
    }

    TEST("/dev/mixer open");
    {
        int fd = open("/dev/mixer", O_RDWR);
        if (fd >= 0) { close(fd); PASS(); }
        else FAIL("open /dev/mixer");
    }

    TEST("/dev/dsp fstat is char device");
    {
        int fd = open("/dev/dsp", O_WRONLY);
        if (fd < 0) FAIL("open");
        else {
            struct stat st;
            if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) PASS();
            else FAIL("not char dev");
            close(fd);
        }
    }

    SUMMARY("test-dev-dsp-presence");
    return fails ? 1 : 0;
}
