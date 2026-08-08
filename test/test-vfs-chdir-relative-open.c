/* test-vfs-chdir-relative-open.c — chdir + relative open coherence
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * In legacy mode, documents host-cwd behavior. With rooted binds this
 * verifies virtual CWD + relative open share one resolver.
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
    printf("test-vfs-chdir-relative-open\n");

    /* Create a temp file in /tmp and chdir there, then open by relative name. */
    char tmpl[] = "/tmp/hl-vfs-XXXXXX";
    int tfd = mkstemp(tmpl);
    TEST("mkstemp");
    if (tfd < 0) { FAIL("mkstemp"); SUMMARY("test-vfs-chdir-relative-open"); return 1; }
    PASS();
    const char *msg = "vfs-ok\n";
    write(tfd, msg, strlen(msg));
    close(tfd);

    char dir[256];
    snprintf(dir, sizeof(dir), "%s", tmpl);
    char *slash = strrchr(dir, '/');
    char *base = slash ? slash + 1 : tmpl;
    if (slash) *slash = '\0';

    TEST("chdir to parent");
    if (chdir(dir) == 0) PASS();
    else FAIL("chdir");

    TEST("relative open");
    {
        int fd = open(base, O_RDONLY);
        if (fd >= 0) {
            char buf[32];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) { buf[n] = 0; if (strstr(buf, "vfs-ok")) PASS(); else FAIL("content"); }
            else FAIL("read");
        } else FAIL("open relative");
    }

    unlink(tmpl);
    SUMMARY("test-vfs-chdir-relative-open");
    return fails ? 1 : 0;
}
