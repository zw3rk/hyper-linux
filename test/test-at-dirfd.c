/* test-at-dirfd.c — *at() ops must honour their dirfd, not the process CWD
 *
 * Regression: five path ops (renameat2, linkat, fchmodat, fchownat,
 * utimensat) were changed to resolve in BOTH fs modes, but they all then
 * passed the resolved path with AT_FDCWD. That is correct in rooted mode,
 * where resolution yields an absolute host path — but legacy mode only
 * redirects ABSOLUTE paths through --sysroot and returns relative ones
 * verbatim. Pairing a relative path with AT_FDCWD reinterprets it against
 * the process CWD, so each op silently hit a same-named file in the wrong
 * directory and returned 0.
 *
 * The test runs with CWD = "b" and a dirfd on "a", both holding a file
 * called "target":
 *   (-) an op via the dirfd must not touch b's copy
 *   (-) a name that exists only in the CWD must fail with ENOENT via the
 *       dirfd — under the bug it succeeded
 *   (+) the op must actually take effect on a's copy
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
#include <sys/syscall.h>

#ifndef SYS_renameat2
#define SYS_renameat2 276 /* aarch64 */
#endif

#define DIR_A "atd-a"
#define DIR_B "atd-b"

static int make_file(const char *path, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    close(fd);
    /* open() honours umask; the tests compare exact modes. */
    return chmod(path, mode);
}

int main(void) {
    int passes = 0, fails = 0;
    printf("test-at-dirfd: *at() ops honour their dirfd\n");

    mkdir(DIR_A, 0755);
    mkdir(DIR_B, 0755);
    if (make_file(DIR_A "/target", 0644) < 0 ||
        make_file(DIR_B "/target", 0644) < 0 ||
        make_file(DIR_B "/only-in-cwd", 0644) < 0) {
        printf("FAIL: setup: %s\n", strerror(errno));
        return 1;
    }

    int dfd = open(DIR_A, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        printf("FAIL: open(%s, O_DIRECTORY): %s\n", DIR_A, strerror(errno));
        return 1;
    }
    if (chdir(DIR_B) != 0) {
        printf("FAIL: chdir(%s): %s\n", DIR_B, strerror(errno));
        return 1;
    }
    /* From here: CWD is b, dfd is a. "target" names a different file
     * through each of them. */

    TEST("fchmodat(dirfd) changes the dirfd's file, not the CWD's");
    {
        struct stat sa, sb;
        if (fchmodat(dfd, "target", 0600, 0) != 0) {
            FAILF("fchmodat: %s", strerror(errno));
        } else if (fstatat(dfd, "target", &sa, 0) != 0 ||
                   stat("target", &sb) != 0) {
            FAILF("stat: %s", strerror(errno));
        } else if ((sa.st_mode & 0777) != 0600) {
            FAILF("dirfd file is 0%o, want 0600", sa.st_mode & 0777);
        } else if ((sb.st_mode & 0777) != 0644) {
            FAILF("CWD file was modified instead (now 0%o)",
                  sb.st_mode & 0777);
        } else PASS();
    }

    TEST("fchmodat(dirfd) on a CWD-only name fails with ENOENT");
    {
        errno = 0;
        if (fchmodat(dfd, "only-in-cwd", 0600, 0) == 0)
            FAIL("succeeded — it resolved against the CWD");
        else if (errno == ENOENT) PASS();
        else FAILF("errno %s, want ENOENT", strerror(errno));
    }

    TEST("fchownat(dirfd) on a CWD-only name fails with ENOENT");
    {
        errno = 0;
        if (fchownat(dfd, "only-in-cwd", getuid(), getgid(), 0) == 0)
            FAIL("succeeded — it resolved against the CWD");
        else if (errno == ENOENT) PASS();
        else FAILF("errno %s, want ENOENT", strerror(errno));
    }

    TEST("utimensat(dirfd) stamps the dirfd's file, not the CWD's");
    {
        struct timespec ts[2];
        ts[0].tv_sec = 1000000000; ts[0].tv_nsec = 0;   /* atime */
        ts[1].tv_sec = 1000000000; ts[1].tv_nsec = 0;   /* mtime */
        struct stat sa, sb;
        if (utimensat(dfd, "target", ts, 0) != 0) {
            FAILF("utimensat: %s", strerror(errno));
        } else if (fstatat(dfd, "target", &sa, 0) != 0 ||
                   stat("target", &sb) != 0) {
            FAILF("stat: %s", strerror(errno));
        } else if (sa.st_mtime != 1000000000) {
            FAILF("dirfd file mtime %lld, want 1000000000",
                  (long long)sa.st_mtime);
        } else if (sb.st_mtime == 1000000000) {
            FAIL("CWD file was stamped instead");
        } else PASS();
    }

    TEST("utimensat(dirfd) on a CWD-only name fails with ENOENT");
    {
        errno = 0;
        if (utimensat(dfd, "only-in-cwd", NULL, 0) == 0)
            FAIL("succeeded — it resolved against the CWD");
        else if (errno == ENOENT) PASS();
        else FAILF("errno %s, want ENOENT", strerror(errno));
    }

    TEST("linkat(dirfd) creates the link under the dirfd, not the CWD");
    {
        struct stat st;
        if (linkat(dfd, "target", dfd, "link", 0) != 0) {
            FAILF("linkat: %s", strerror(errno));
        } else if (fstatat(dfd, "link", &st, 0) != 0) {
            FAILF("link missing under dirfd: %s", strerror(errno));
        } else if (stat("link", &st) == 0) {
            FAIL("link was created in the CWD instead");
        } else PASS();
    }

    TEST("renameat2(dirfd) renames under the dirfd, not the CWD");
    {
        long rc = syscall(SYS_renameat2, dfd, "target", dfd, "moved", 0);
        struct stat st;
        if (rc != 0) {
            FAILF("renameat2: %s", strerror(errno));
        } else if (fstatat(dfd, "moved", &st, 0) != 0) {
            FAILF("renamed file missing under dirfd: %s", strerror(errno));
        } else if (fstatat(dfd, "target", &st, 0) == 0) {
            FAIL("old name still present under dirfd");
        } else if (stat("target", &st) != 0) {
            FAIL("the CWD's file was renamed instead");
        } else PASS();
    }

    /* (V13) An ABSOLUTE path ignores dirfd on Linux — a bad/closed dirfd
     * must not make it EBADF. */
    TEST("fchmodat(bad dirfd, absolute path) ignores the dirfd");
    {
        /* Fresh file under DIR_A (earlier subtests renamed "target" away). */
        int ff = openat(dfd, "v13f", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ff >= 0) close(ff);
        char cwd[512], abs[600];
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(abs, sizeof(abs), "%s/../%s/v13f", cwd, DIR_A);
            int badfd = 999;   /* never opened */
            errno = 0;
            int r = fchmodat(badfd, abs, 0640, 0);
            if (r == 0) PASS();
            else FAILF("fchmodat(absolute) with bad dirfd: %s", strerror(errno));
        } else { FAIL("getcwd"); }
        unlinkat(dfd, "v13f", 0);
    }

    /* (V14) RENAME_EXCHANGE via a real dirfd (was EINVAL in legacy mode). */
    TEST("renameat2(RENAME_EXCHANGE) works via a dirfd");
    {
        /* Create two files under dfd (DIR_A): x and y with distinct content. */
        int fx = openat(dfd, "xk", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fx >= 0) { write(fx, "XX", 2); close(fx); }
        int fy = openat(dfd, "yk", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fy >= 0) { write(fy, "YY", 2); close(fy); }
        long rc = syscall(SYS_renameat2, dfd, "xk", dfd, "yk", 2 /*EXCHANGE*/);
        if (rc != 0) {
            FAILF("renameat2 EXCHANGE: %s", strerror(errno));
        } else {
            char b[4] = {0};
            int f = openat(dfd, "xk", O_RDONLY);
            ssize_t n = (f >= 0) ? read(f, b, 3) : -1;
            if (f >= 0) close(f);
            if (n == 2 && b[0] == 'Y') PASS();   /* xk now holds YY */
            else FAILF("after exchange xk=%.2s (want YY)", b);
        }
        unlinkat(dfd, "xk", 0); unlinkat(dfd, "yk", 0);
    }

    /* (V14) RENAME_NOREPLACE via a real dirfd must fail EEXIST if dest exists. */
    TEST("renameat2(RENAME_NOREPLACE) via a dirfd rejects an existing dest");
    {
        int fa = openat(dfd, "na", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fa >= 0) close(fa);
        int fb = openat(dfd, "nb", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fb >= 0) close(fb);
        errno = 0;
        long rc = syscall(SYS_renameat2, dfd, "na", dfd, "nb", 1 /*NOREPLACE*/);
        if (rc == 0) FAIL("overwrote an existing dest with NOREPLACE");
        else if (errno == EEXIST) PASS();
        else FAILF("errno %s, want EEXIST", strerror(errno));
        unlinkat(dfd, "na", 0); unlinkat(dfd, "nb", 0);
    }

    close(dfd);
    unlink("target"); unlink("only-in-cwd");
    if (chdir("..") == 0) {
        unlink(DIR_A "/target"); unlink(DIR_A "/moved"); unlink(DIR_A "/link");
        rmdir(DIR_A); rmdir(DIR_B);
    }

    SUMMARY("test-at-dirfd");
    return fails ? 1 : 0;
}
