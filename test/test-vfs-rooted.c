/* test-vfs-rooted.c — rooted mode bind + create-under-bind + relative open
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * MUST be launched with:
 *   hl --fs-mode=rooted --bind <tmpdir>:/home/user --guest-cwd /home/user ...
 */
#define _GNU_SOURCE
#include "test-harness.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

int main(void) {
    int passes = 0, fails = 0;
    printf("test-vfs-rooted: bind mounts + create (requires --fs-mode=rooted)\n");

    /* Create under bind: file must not need to pre-exist */
    TEST("create O_CREAT under guest home");
    {
        int fd = open("/home/user/playlist-new.m3u",
                      O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) FAIL("create");
        else {
            const char *line = "#EXTM3U\n";
            if (write(fd, line, strlen(line)) > 0) PASS();
            else FAIL("write");
            close(fd);
        }
    }

    TEST("stat created file");
    {
        struct stat st;
        if (stat("/home/user/playlist-new.m3u", &st) == 0 && st.st_size > 0)
            PASS();
        else FAIL("stat");
    }

    TEST("chdir virtual + relative open");
    {
        if (chdir("/home/user") != 0) FAIL("chdir");
        else {
            int fd = open("playlist-new.m3u", O_RDONLY);
            if (fd >= 0) { close(fd); PASS(); }
            else FAIL("relative open");
        }
    }

    TEST("getcwd reports guest path");
    {
        char buf[4096];
        if (getcwd(buf, sizeof(buf)) && strstr(buf, "/home/user"))
            PASS();
        else FAIL(buf);
    }

    TEST("unlink under bind");
    {
        if (unlink("/home/user/playlist-new.m3u") == 0) PASS();
        else FAIL("unlink");
    }

    SUMMARY("test-vfs-rooted");
    return fails ? 1 : 0;
}
