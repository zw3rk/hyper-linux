/* test-vfs-dirfd.c — *at() syscalls against a real dirfd in rooted mode
 *
 * Regression: hl_vfs_resolve_at() resolved a real dirfd only through
 * of->guest_path_hint, which was never assigned anywhere in the tree, so
 * every *at() call with a dirfd other than AT_FDCWD returned ENOTDIR and
 * fchdir() returned ENOENT. That broke openat()-based directory walks
 * (fts, rm -r, find, du) in what is now the default fs mode.
 *
 * Run with:
 *   hl --fs-mode=rooted --bind <tmp>:/home/user --guest-cwd /home/user
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
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

int main(void) {
    int passes = 0, fails = 0;
    printf("test-vfs-dirfd: *at() with a real dirfd (rooted mode)\n");

    /* Build a subdirectory with one file in the guest cwd. */
    mkdir("sub", 0755);
    int f = open("sub/somefile", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) {
        printf("FAIL: cannot create sub/somefile: %s\n", strerror(errno));
        return 1;
    }
    write(f, "FILEDATA-OK", 11);
    close(f);

    int d = open("sub", O_RDONLY | O_DIRECTORY);
    if (d < 0) {
        printf("FAIL: open(sub, O_DIRECTORY): %s\n", strerror(errno));
        return 1;
    }

    TEST("openat(dirfd, relative)");
    {
        int fd = openat(d, "somefile", O_RDONLY);
        if (fd < 0) FAILF("openat: %s", strerror(errno));
        else {
            char buf[32] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n == 11 && strcmp(buf, "FILEDATA-OK") == 0) PASS();
            else FAILF("read %zd bytes: %s", n, buf);
        }
    }

    TEST("fstatat(dirfd, relative)");
    {
        struct stat st;
        if (fstatat(d, "somefile", &st, 0) == 0 && st.st_size == 11) PASS();
        else FAILF("fstatat: %s", strerror(errno));
    }

    TEST("faccessat(dirfd, relative)");
    if (faccessat(d, "somefile", R_OK, 0) == 0) PASS();
    else FAILF("faccessat: %s", strerror(errno));

    TEST("mkdirat + unlinkat(AT_REMOVEDIR)");
    {
        if (mkdirat(d, "newdir", 0755) != 0)
            FAILF("mkdirat: %s", strerror(errno));
        else if (unlinkat(d, "newdir", AT_REMOVEDIR) != 0)
            FAILF("unlinkat rmdir: %s", strerror(errno));
        else PASS();
    }

    TEST("symlinkat + readlinkat");
    {
        unlinkat(d, "lnk", 0);
        if (symlinkat("somefile", d, "lnk") != 0)
            FAILF("symlinkat: %s", strerror(errno));
        else {
            char buf[64] = {0};
            ssize_t n = readlinkat(d, "lnk", buf, sizeof(buf) - 1);
            if (n == 8 && strcmp(buf, "somefile") == 0) PASS();
            else FAILF("readlinkat n=%zd buf=%s", n, buf);
            unlinkat(d, "lnk", 0);
        }
    }

    TEST("renameat(dirfd)");
    {
        if (renameat(d, "somefile", d, "renamed") != 0)
            FAILF("renameat: %s", strerror(errno));
        else if (faccessat(d, "renamed", R_OK, 0) != 0)
            FAILF("renamed missing: %s", strerror(errno));
        else {
            renameat(d, "renamed", d, "somefile");
            PASS();
        }
    }

    TEST("unlinkat(dirfd, file)");
    {
        int t = openat(d, "victim", O_WRONLY | O_CREAT, 0644);
        if (t < 0) FAILF("create victim: %s", strerror(errno));
        else {
            close(t);
            if (unlinkat(d, "victim", 0) == 0) PASS();
            else FAILF("unlinkat: %s", strerror(errno));
        }
    }

    TEST("fchdir(dirfd)");
    {
        char before[512] = {0}, after[512] = {0};
        if (!getcwd(before, sizeof(before))) FAIL("getcwd before");
        else if (fchdir(d) != 0) FAILF("fchdir: %s", strerror(errno));
        else if (!getcwd(after, sizeof(after))) FAIL("getcwd after");
        else if (strcmp(before, after) == 0)
            FAILF("cwd unchanged after fchdir: %s", after);
        else {
            /* A relative open must now resolve inside sub/ */
            int fd = open("somefile", O_RDONLY);
            if (fd >= 0) { close(fd); PASS(); }
            else FAILF("open after fchdir: %s", strerror(errno));
            if (chdir(before) != 0) printf("(warn: chdir back failed) ");
        }
    }

    /* Control: AT_FDCWD must keep working. */
    TEST("openat(AT_FDCWD, relative) control");
    {
        int fd = openat(AT_FDCWD, "sub/somefile", O_RDONLY);
        if (fd >= 0) { close(fd); PASS(); }
        else FAILF("openat AT_FDCWD: %s", strerror(errno));
    }

    close(d);
    unlink("sub/somefile");
    rmdir("sub");

    SUMMARY("test-vfs-dirfd");
    return fails ? 1 : 0;
}
