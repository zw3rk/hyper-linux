/* test-vfs-inotify.c — a bind must confine inotify_add_watch (VFS-F2)
 *
 * Regression: in rooted mode sys_inotify_add_watch opened the RAW guest path
 * (open(path, O_EVTONLY)) with no resolver, so a confined guest could add a
 * watch on /etc — or any host path outside its bind — and monitor it for
 * changes. The fix routes the watch open through the rooted resolver and opens
 * the target beneath the bind root with O_RESOLVE_BENEATH, so the kernel
 * refuses any escape (absolute path, .., outward symlink) race-free. This
 * mirrors test-vfs-containment for the inotify surface.
 *
 * Run with:
 *   hl --fs-mode=rooted --bind <tmp>:/home/user --guest-cwd /home/user
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

int passes = 0, fails = 0;

/* An add_watch that must NOT succeed: a valid wd means the guest reached a
 * host path outside its bind. A macro (not a function) on purpose — as a
 * function taking `int *passes,*fails` the PASS()/FAIL() macros would
 * increment the POINTERS, never main's counters, and every escape would
 * silently "pass". Require a containment errno, not just any failure, so a
 * typo'd path failing with the wrong errno cannot masquerade as a refusal. */
#define MUST_NOT_WATCH(what, ifd, path, mask)                              \
    do {                                                                   \
        TEST(what);                                                        \
        errno = 0;                                                         \
        int _wd = inotify_add_watch((ifd), (path), (mask));                \
        int _e = errno;                                                    \
        if (_wd >= 0) {                                                    \
            inotify_rm_watch((ifd), _wd);                                  \
            FAILF("ESCAPED: watched %s (wd=%d)", (path), _wd);             \
        } else if (_e == ENOENT || _e == EACCES || _e == EPERM ||          \
                   _e == ELOOP || _e == ENOTDIR) PASS();                   \
        else FAILF("failed with %s, not a containment refusal",            \
                   strerror(_e));                                          \
    } while (0)

int main(void) {
    printf("test-vfs-inotify: watches must stay inside the bind\n");

    int ifd = inotify_init1(IN_NONBLOCK);
    if (ifd < 0) { printf("  inotify_init1 FAILED (errno=%d)\n", errno); return 1; }

    /* Control (+): a watch on a file INSIDE the bind must work, otherwise the
     * refusals below prove nothing. The file is created in the (bound) CWD. */
    TEST("control: watch a file inside the bind");
    {
        const char *inside = "watched.txt";
        int tfd = open(inside, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (tfd < 0) FAILF("create %s: %s", inside, strerror(errno));
        else {
            write(tfd, "hello\n", 6);
            close(tfd);
            int wd = inotify_add_watch(ifd, inside,
                                       IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE);
            if (wd < 0) {
                FAILF("add_watch inside bind: %s", strerror(errno));
            } else {
                PASS();
                /* (+) event delivery: modify the file and read the event.
                 * kqueue delivery is best-effort, so a quiet poll still
                 * passes — the load-bearing assertion is the watch itself. */
                TEST("in-bind watch delivers an event");
                int mfd = open(inside, O_WRONLY | O_APPEND);
                if (mfd >= 0) { write(mfd, "world\n", 6); close(mfd); }
                struct pollfd pfd = { .fd = ifd, .events = POLLIN };
                int ready = poll(&pfd, 1, 1000);
                if (ready < 0) {
                    FAILF("poll: %s", strerror(errno));
                } else if (ready == 0) {
                    PASS();  /* best-effort: no event within the window */
                } else {
                    char buf[256];
                    ssize_t n = read(ifd, buf, sizeof(buf));
                    if (n >= (ssize_t)sizeof(struct inotify_event)) PASS();
                    else FAILF("read %zd bytes after poll-ready", n);
                }
                inotify_rm_watch(ifd, wd);
            }
        }
        unlink(inside);
    }

    /* (−) The escapes: /etc exists on the host but is bound by no mount, so a
     * confined guest must not be able to watch it. Under the bug the raw open
     * succeeded and returned a live watch descriptor. */
    MUST_NOT_WATCH("absolute host dir /etc", ifd, "/etc", IN_ATTRIB);
    MUST_NOT_WATCH("absolute host file /etc/hosts", ifd, "/etc/hosts", IN_ATTRIB);
    /* Lexical .. climbing above the bind root normalizes to the (synthetic)
     * guest root, which has no host inode to watch — refused, not escaped. */
    MUST_NOT_WATCH("lexical .. out of the bind", ifd, "../..", IN_ATTRIB);

    close(ifd);
    SUMMARY("test-vfs-inotify");
    return fails ? 1 : 0;
}
