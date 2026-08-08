/* test-fs-semantics.c — symlink and read-only-mount semantics
 *
 * (-) The l*xattr family resolved the path WITH symlink following, then
 *     passed XATTR_NOFOLLOW to the host call. The host was already looking
 *     at the target, so lgetxattr/lsetxattr/lremovexattr behaved exactly
 *     like their non-l forms — the "l" prefix did nothing at all.
 *
 * (-) setxattr, removexattr and utimensat never asked the resolver which
 *     mount they landed on, so their read-only/virtual guard was missing
 *     entirely: a read-only bind could still have extended attributes
 *     written and timestamps rewritten.
 *
 * (-) open(O_CREAT|O_EXCL) followed a final symlink. On Linux it never
 *     does — the whole point of O_EXCL is that an existing name, symlink
 *     included, fails with EEXIST. Following it meant the host open acted
 *     on the target, so a link naming a file that does not exist yet made
 *     hl create THAT file, wherever it pointed. The classic symlink attack.
 *
 * Run with: hl --fs-mode=rooted --bind <tmp>:/home/user --guest-cwd /home/user
 * (/nix/store is auto-bound read-only by the default rooted profile.)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#define RO_PATH "/nix/store"

int main(void) {
    int passes = 0, fails = 0;
    printf("test-fs-semantics: symlink and read-only-mount semantics\n");

    /* ---- O_CREAT|O_EXCL must not follow the final symlink ---- */
    unlink("excl-target");
    unlink("excl-link");
    TEST("open(O_CREAT|O_EXCL) on a symlink is EEXIST");
    {
        if (symlink("excl-target", "excl-link") != 0) {
            FAILF("symlink: %s", strerror(errno));
        } else {
            errno = 0;
            int fd = open("excl-link", O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd >= 0) {
                FAIL("created through the link");
                close(fd);
            } else if (errno == EEXIST) PASS();
            else FAILF("errno %s, want EEXIST", strerror(errno));
        }
    }

    TEST("...and it did not create the link's target");
    {
        struct stat st;
        if (lstat("excl-target", &st) != 0 && errno == ENOENT) PASS();
        else FAIL("the target was created — symlink was followed");
    }
    unlink("excl-target");
    unlink("excl-link");

    /* (+) O_CREAT without O_EXCL still follows, as Linux does. */
    TEST("open(O_CREAT) without O_EXCL still follows the symlink");
    {
        unlink("cr-target"); unlink("cr-link");
        symlink("cr-target", "cr-link");
        int fd = open("cr-link", O_CREAT | O_WRONLY, 0644);
        struct stat st;
        if (fd < 0) FAILF("open: %s", strerror(errno));
        else {
            close(fd);
            if (lstat("cr-target", &st) == 0) PASS();
            else FAIL("target not created — O_CREAT stopped following");
        }
        unlink("cr-target"); unlink("cr-link");
    }

    /* ---- l*xattr must act on the link, not its target ---- */
    TEST("lgetxattr looks at the link, not the target");
    {
        unlink("x-target"); unlink("x-link");
        int fd = open("x-target", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        if (symlink("x-target", "x-link") != 0) {
            FAILF("symlink: %s", strerror(errno));
        } else if (setxattr("x-target", "user.hltest", "V", 1, 0) != 0) {
            /* Not all filesystems carry user xattrs; skip rather than lie. */
            printf("(skipped: setxattr: %s) ", strerror(errno));
            PASS();
        } else {
            char buf[8];
            /* Following the link must see the target's attribute... */
            ssize_t got = getxattr("x-link", "user.hltest", buf, sizeof(buf));
            /* ...and NOT following it must not. */
            errno = 0;
            ssize_t lgot = lgetxattr("x-link", "user.hltest", buf,
                                     sizeof(buf));
            if (got != 1)
                FAILF("getxattr through link: %zd (%s)", got, strerror(errno));
            else if (lgot >= 0)
                FAIL("lgetxattr saw the target's attribute — nofollow is dead");
            else PASS();
        }
        unlink("x-target"); unlink("x-link");
    }

    /* ---- read-only mount guards ---- */
    TEST("setxattr on a read-only mount is EROFS");
    {
        errno = 0;
        int r = setxattr(RO_PATH, "user.hltest", "V", 1, 0);
        if (r == 0) FAIL("wrote an xattr onto a read-only mount");
        else if (errno == EROFS) PASS();
        else FAILF("errno %s, want EROFS", strerror(errno));
    }

    TEST("removexattr on a read-only mount is EROFS");
    {
        errno = 0;
        int r = removexattr(RO_PATH, "user.hltest");
        if (r == 0) FAIL("removed an xattr from a read-only mount");
        else if (errno == EROFS) PASS();
        else FAILF("errno %s, want EROFS", strerror(errno));
    }

    TEST("utimensat on a read-only mount is EROFS");
    {
        struct timespec ts[2];
        ts[0].tv_sec = ts[1].tv_sec = 1000000000;
        ts[0].tv_nsec = ts[1].tv_nsec = 0;
        errno = 0;
        int r = utimensat(AT_FDCWD, RO_PATH, ts, 0);
        if (r == 0) FAIL("rewrote timestamps on a read-only mount");
        else if (errno == EROFS) PASS();
        else FAILF("errno %s, want EROFS", strerror(errno));
    }

    /* (+) The same operations must still work on a writable mount. */
    TEST("utimensat still works on a writable mount");
    {
        int fd = open("stampme", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        struct timespec ts[2];
        ts[0].tv_sec = ts[1].tv_sec = 1000000000;
        ts[0].tv_nsec = ts[1].tv_nsec = 0;
        struct stat st;
        if (utimensat(AT_FDCWD, "stampme", ts, 0) != 0)
            FAILF("utimensat: %s", strerror(errno));
        else if (stat("stampme", &st) != 0)
            FAILF("stat: %s", strerror(errno));
        else if (st.st_mtime == 1000000000) PASS();
        else FAILF("mtime %lld", (long long)st.st_mtime);
        unlink("stampme");
    }

    SUMMARY("test-fs-semantics");
    return fails ? 1 : 0;
}
