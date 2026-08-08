/* test-dev-bare-name.c — a relative file named like a device is a real file
 *
 * Regression: basename_dev() treated any slash-free path as a /dev node
 * name, so open("random") returned /dev/urandom, open("null") swallowed
 * writes, and stat("audio") reported a character device — for ordinary
 * files sitting in the working directory.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

/* Device names the registry knows about; each must stay openable as a
 * plain relative file when one exists in the cwd. */
static const char *kNames[] = { "random", "null", "zero", "audio", "mixer" };

static int write_file(const char *name, const char *data) {
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t w = write(fd, data, strlen(data));
    close(fd);
    return (w == (ssize_t)strlen(data)) ? 0 : -1;
}

int main(void) {
    int passes = 0, fails = 0;
    printf("test-dev-bare-name: relative files must not hit /dev\n");

    char dir[] = "/tmp/hl-devname-XXXXXX";
    if (!mkdtemp(dir)) {
        printf("FAIL: mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    if (chdir(dir) != 0) {
        printf("FAIL: chdir(%s): %s\n", dir, strerror(errno));
        return 1;
    }

    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        const char *name = kNames[i];
        const char *want = "REALFILE";

        TEST(name);
        if (write_file(name, want) != 0) {
            FAILF("could not create %s: %s", name, strerror(errno));
            continue;
        }

        /* (+) reading the bare name must yield the file's own contents */
        char buf[64];
        memset(buf, 0, sizeof(buf));
        int fd = open(name, O_RDONLY);
        if (fd < 0) {
            FAILF("open(%s): %s", name, strerror(errno));
            continue;
        }
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (n != (ssize_t)strlen(want) || strcmp(buf, want) != 0) {
            FAILF("%s: read %zd bytes %.16s, expected %s", name, n, buf, want);
            continue;
        }

        /* (-) and it must be a regular file, not a char device */
        struct stat st;
        if (stat(name, &st) != 0) { FAILF("stat(%s)", name); continue; }
        if (!S_ISREG(st.st_mode)) {
            FAILF("%s: mode %o is not a regular file", name, st.st_mode);
            continue;
        }
        PASS();
    }

    /* Absolute device paths must still resolve to the virtual devices. */
    TEST("/dev/null still a device");
    {
        struct stat st;
        int fd = open("/dev/null", O_WRONLY);
        if (fd < 0) FAILF("open /dev/null: %s", strerror(errno));
        else {
            close(fd);
            if (stat("/dev/null", &st) == 0 && S_ISCHR(st.st_mode)) PASS();
            else FAIL("/dev/null is not a char device");
        }
    }

    TEST("/dev/zero still readable");
    {
        char b[4] = { 1, 1, 1, 1 };
        int fd = open("/dev/zero", O_RDONLY);
        if (fd < 0) FAILF("open /dev/zero: %s", strerror(errno));
        else {
            ssize_t n = read(fd, b, sizeof(b));
            close(fd);
            if (n == 4 && !b[0] && !b[1] && !b[2] && !b[3]) PASS();
            else FAILF("read %zd bytes, not zeroed", n);
        }
    }

    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++)
        unlink(kNames[i]);
    if (chdir("/") == 0) rmdir(dir);

    SUMMARY("test-dev-bare-name");
    return fails ? 1 : 0;
}
