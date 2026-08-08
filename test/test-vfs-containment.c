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
#define MUST_NOT_OPEN(what, path)                                        \
    do {                                                                 \
        TEST(what);                                                      \
        int _fd = open((path), O_RDONLY);                                \
        if (_fd >= 0) { close(_fd); FAILF("ESCAPED: opened %s", (path)); }\
        else PASS();                                                     \
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

    /* Interior symlink to the host root. */
    unlink("esc");
    if (symlink("/", "esc") == 0)
        MUST_NOT_OPEN("interior symlink to /", "esc/etc/hosts");
    else
        printf("  %-30s SKIP (symlink: %s)\n", "interior symlink to /",
               strerror(errno));

    /* Interior symlink straight at a host directory. */
    unlink("esc2");
    if (symlink("/etc", "esc2") == 0)
        MUST_NOT_OPEN("interior symlink to /etc", "esc2/hosts");
    else
        printf("  %-30s SKIP (symlink: %s)\n", "interior symlink to /etc",
               strerror(errno));

    /* Lexical traversal must stay contained (this already worked —
     * normalize_guest runs before mount selection — so it guards against a
     * regression in that ordering). */
    MUST_NOT_OPEN("lexical .. traversal", "../../../../etc/hosts");
    MUST_NOT_OPEN("absolute host path", "/etc/hosts");

    unlink("esc"); unlink("esc2"); unlink("inside.txt");
    SUMMARY("test-vfs-containment");
    return fails ? 1 : 0;
}
