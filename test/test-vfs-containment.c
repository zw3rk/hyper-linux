/* test-vfs-containment.c — a bind must not be escapable via symlinks
 *
 * Regression: hl_vfs_resolve_at() builds the host path by string
 * concatenation and only inspects the FINAL component for symlinks. Every
 * interior component was resolved by the host kernel with host semantics,
 * so a symlink planted inside a bind — which the guest can create itself
 * with symlinkat() — escaped the mount entirely:
 *
 *     ln -s / esc; cat /home/user/esc/etc/hosts   -> host /etc/hosts
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

/* An open that must NOT succeed: a valid fd means the guest reached a host
 * file outside its bind.
 *
 * This is a macro on purpose. As a function taking `int *passes, int *fails`
 * the PASS()/FAIL() macros would expand to `passes++`/`fails++` on the
 * POINTERS — incrementing the pointer, never the caller's counters — so the
 * summary would report 0 failures no matter what happened. */
/* Any failure used to count as containment, so a typo'd path that failed
 * with ENOENT for the wrong reason — or an EMFILE — passed just as happily
 * as a real refusal. Require a containment errno. */
#define MUST_NOT_OPEN(what, path)                                        \
    do {                                                                 \
        TEST(what);                                                      \
        errno = 0;                                                       \
        int _fd = open((path), O_RDONLY);                                \
        int _e = errno;                                                  \
        if (_fd >= 0) { close(_fd); FAILF("ESCAPED: opened %s", (path)); }\
        else if (_e == EACCES || _e == ENOENT || _e == EPERM ||          \
                 _e == ELOOP || _e == ENOTDIR) PASS();                   \
        else FAILF("failed with %s, not a containment refusal",          \
                   strerror(_e));                                        \
    } while (0)

int main(void) {
    int passes = 0, fails = 0;
    printf("test-vfs-containment: binds must not be escapable\n");

    /* Control: an ordinary file inside the bind must work, otherwise the
     * rest of this test proves nothing. */
    TEST("control: file inside the bind is readable");
    {
        int fd = open("inside.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) FAILF("create: %s", strerror(errno));
        else {
            write(fd, "OK", 2);
            close(fd);
            char buf[8] = {0};
            int rd = open("inside.txt", O_RDONLY);
            if (rd < 0) FAILF("reopen: %s", strerror(errno));
            else {
                ssize_t n = read(rd, buf, sizeof(buf) - 1);
                close(rd);
                if (n == 2 && strcmp(buf, "OK") == 0) PASS();
                else FAILF("read %zd bytes: %s", n, buf);
            }
        }
    }

    /* A failed symlink() used to print SKIP and count NOTHING, so the two
     * checks that matter most could vanish and the suite still reported
     * zero failures — which is exactly what happened while the containment
     * check was over-blocking symlink creation itself. Creating a link
     * inside a bind is legal; if it fails, that IS the bug. */
    TEST("a symlink can be created inside the bind");
    unlink("esc");
    if (symlink("/", "esc") == 0) PASS();
    else FAILF("symlink: %s", strerror(errno));
    MUST_NOT_OPEN("interior symlink to /", "esc/etc/hosts");

    unlink("esc2");
    TEST("a second symlink can be created");
    if (symlink("/etc", "esc2") == 0) PASS();
    else FAILF("symlink: %s", strerror(errno));
    MUST_NOT_OPEN("interior symlink to /etc", "esc2/hosts");

    /* The escape targets above are host paths that may or may not exist,
     * so on their own they cannot distinguish "refused" from "absent".
     * ../secret.txt is created by the harness immediately outside the bind
     * root, so it definitely exists — and must still be unreachable. */
    unlink("esc3");
    TEST("a symlink to the bind's parent can be created");
    if (symlink("..", "esc3") == 0) PASS();
    else FAILF("symlink: %s", strerror(errno));
    MUST_NOT_OPEN("symlink to the bind parent", "esc3/secret.txt");
    MUST_NOT_OPEN("lexical .. to the bind parent", "../secret.txt");

    /* Lexical traversal must stay contained (this already worked —
     * normalize_guest runs before mount selection — so it guards against a
     * regression in that ordering). */
    MUST_NOT_OPEN("lexical .. traversal", "../../../../etc/hosts");
    MUST_NOT_OPEN("absolute host path", "/etc/hosts");

    unlink("esc"); unlink("esc2"); unlink("esc3");
    unlink("inside.txt");
    SUMMARY("test-vfs-containment");
    return fails ? 1 : 0;
}
