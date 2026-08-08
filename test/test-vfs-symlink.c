/* test-vfs-symlink.c — the final path component must not be followed blindly
 *
 * Regression: hl_vfs_resolve_at() took a follow_final_symlink parameter and
 * immediately discarded it with (void). Every caller passed 1, so the final
 * component was always followed. Consequences, all data-visible:
 *
 *   rm link        -> deleted the TARGET and left the dangling link
 *   readlink(link) -> EINVAL (it had already become a regular file)
 *   lstat(link)    -> reported the target, so S_ISLNK was never true
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
#include <sys/stat.h>

int main(void) {
    int passes = 0, fails = 0;
    printf("test-vfs-symlink: final-component symlink semantics\n");

    unlink("lnk"); unlink("target");
    int f = open("target", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) { printf("FAIL: create target: %s\n", strerror(errno)); return 1; }
    write(f, "TARGETDATA", 10);
    close(f);
    if (symlink("target", "lnk") != 0) {
        printf("FAIL: symlink: %s\n", strerror(errno));
        return 1;
    }

    TEST("lstat reports a symlink");
    {
        struct stat st;
        if (lstat("lnk", &st) != 0) FAILF("lstat: %s", strerror(errno));
        else if (S_ISLNK(st.st_mode)) PASS();
        else FAILF("mode %o is not a symlink", st.st_mode);
    }

    TEST("stat follows to the target");
    {
        struct stat st;
        if (stat("lnk", &st) != 0) FAILF("stat: %s", strerror(errno));
        else if (S_ISREG(st.st_mode) && st.st_size == 10) PASS();
        else FAILF("mode %o size %lld", st.st_mode, (long long)st.st_size);
    }

    TEST("readlink returns the link text");
    {
        char buf[64] = {0};
        ssize_t n = readlink("lnk", buf, sizeof(buf) - 1);
        if (n == 6 && strcmp(buf, "target") == 0) PASS();
        else FAILF("n=%zd buf=%s", n, buf);
    }

    TEST("open follows to the target");
    {
        int fd = open("lnk", O_RDONLY);
        if (fd < 0) FAILF("open: %s", strerror(errno));
        else {
            char buf[32] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n == 10 && strcmp(buf, "TARGETDATA") == 0) PASS();
            else FAILF("read %zd: %s", n, buf);
        }
    }

    TEST("unlink removes the link, not the target");
    {
        if (unlink("lnk") != 0) FAILF("unlink: %s", strerror(errno));
        else {
            struct stat st;
            int link_gone   = (lstat("lnk", &st) != 0);
            int target_kept = (stat("target", &st) == 0 && st.st_size == 10);
            if (link_gone && target_kept) PASS();
            else FAILF("link_gone=%d target_kept=%d", link_gone, target_kept);
        }
    }

    TEST("rename moves the link itself");
    {
        symlink("target", "lnk2");
        if (rename("lnk2", "lnk3") != 0) FAILF("rename: %s", strerror(errno));
        else {
            struct stat st;
            if (lstat("lnk3", &st) == 0 && S_ISLNK(st.st_mode)) PASS();
            else FAIL("renamed entry is not a symlink");
            unlink("lnk3");
        }
        unlink("lnk2");
    }

    unlink("target");
    SUMMARY("test-vfs-symlink");
    return fails ? 1 : 0;
}
