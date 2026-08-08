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
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
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

static volatile int tt_stop;
static void *tt_flip(void *a) {
    (void)a;
    mkdir("d_real", 0755);
    symlink("..", "d_link");
    while (!tt_stop) {
        rename("d_real", "race"); rename("race", "d_real");
        rename("d_link", "race"); rename("race", "d_link");
    }
    return NULL;
}

/* Race for ../secret.txt (created outside the bind by the harness) through a
 * swapped "race" component. Returns the number of successful outside reads. */
int toctou_escapes(void) {
    pthread_t th;
    tt_stop = 0;
    if (pthread_create(&th, NULL, tt_flip, NULL) != 0) return -1;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int escapes = 0;
    long tries = 0;
    for (;;) {
        int fd = open("race/secret.txt", O_RDONLY);
        if (fd >= 0) {
            char b[64] = {0};
            ssize_t n = read(fd, b, sizeof(b) - 1);
            close(fd);
            if (n > 0 && strstr(b, "SECRET")) escapes++;
        }
        if ((++tries & 0x3FFF) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - t0.tv_sec >= 4) break;
        }
    }
    tt_stop = 1;
    pthread_join(th, NULL);
    unlink("race"); rmdir("d_real"); unlink("d_link");
    return escapes;
}

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

    /* (H5) The rename-swap TOCTOU: a second thread atomically swaps a name
     * between a real dir and a symlink->.. while this thread opens
     * name/secret.txt. String+realpath checking could not close this window
     * (a guest thread drove it to a real escape); O_RESOLVE_BENEATH resolves
     * beneath the bind root in the kernel, so there must be ZERO escapes. */
    TEST("no rename-swap TOCTOU escape (open path)");
    {
        int esc = toctou_escapes();
        if (esc == 0) PASS();
        else FAILF("%d reads of the outside secret", esc);
    }


    unlink("esc"); unlink("esc2"); unlink("esc3");
    unlink("inside.txt");
    SUMMARY("test-vfs-containment");
    return fails ? 1 : 0;
}
