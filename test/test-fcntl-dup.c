/* test-fcntl-dup.c — F_DUPFD / F_GETFL / F_SETFL and dup3 over a special fd
 *
 * Three regressions in the fd-object layer, all silent:
 *
 * (1) F_DUPFD on a descriptor-backed fd was routed to hl_fd_dup(), which
 *     takes the lowest free fd and always clears CLOEXEC. F_DUPFD's arg is
 *     the MINIMUM acceptable fd, and F_DUPFD_CLOEXEC must set the flag.
 *
 * (2) F_GETFL/F_SETFL short-circuited to the open-file's status_flags for
 *     every fd that had one. That is right for OSS descriptors, whose host
 *     alias is an internal transport — but stdio also has an open-file
 *     object, and its status_flags were hard-coded to 0, so F_GETFL
 *     reported stdout as O_RDONLY and F_SETFL never reached the real
 *     descriptor.
 *
 * (3) dup3() from a DESCRIPTOR-backed source onto a live eventfd went
 *     through hl_fd_dup3(), which pre-removes the target and so never
 *     reached fd_alloc_at()'s special-fd cleanup — the only place that
 *     path was handled. eventfd/signalfd/timerfd/inotify state is keyed on
 *     the guest fd NUMBER, so the slot outlived the fd: the next eventfd()
 *     to reuse that number matched the stale slot in eventfd_find() and
 *     inherited its counter and self-pipe. Verified symptom — a fresh
 *     eventfd(0) reads back 5, the buried eventfd's value.
 *     (A plain pipe as the source takes sys_dup3's legacy path, where
 *     fd_alloc_at does retire the target correctly.)
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
#include <stdint.h>
#include <poll.h>
#include <sys/eventfd.h>

int main(void) {
    int passes = 0, fails = 0;
    printf("test-fcntl-dup: F_DUPFD/F_GETFL/F_SETFL + dup3 over a special fd\n");

    int p[2];
    if (pipe(p) != 0) { printf("FAIL: pipe: %s\n", strerror(errno)); return 1; }

    /* fd 1 on purpose: only stdio and the OSS nodes are descriptor-backed,
     * and it was exactly that path which ignored arg. A pipe fd takes the
     * legacy path, which handled F_DUPFD correctly all along. */
    TEST("F_DUPFD honours its minimum-fd argument");
    {
        int d = fcntl(1, F_DUPFD, 100);
        if (d < 0) FAILF("F_DUPFD: %s", strerror(errno));
        else if (d < 100) FAILF("got fd %d, want >= 100", d);
        else { close(d); PASS(); }
    }

    TEST("F_DUPFD clears CLOEXEC on the new fd");
    {
        fcntl(1, F_SETFD, FD_CLOEXEC);
        int d = fcntl(1, F_DUPFD, 100);
        if (d < 0) FAILF("F_DUPFD: %s", strerror(errno));
        else {
            int fl = fcntl(d, F_GETFD);
            if (fl >= 0 && !(fl & FD_CLOEXEC)) PASS();
            else FAILF("F_GETFD = %d, want CLOEXEC clear", fl);
            close(d);
        }
        fcntl(1, F_SETFD, 0);
    }

    TEST("F_DUPFD_CLOEXEC sets CLOEXEC and honours the minimum");
    {
        int d = fcntl(1, F_DUPFD_CLOEXEC, 100);
        if (d < 0) FAILF("F_DUPFD_CLOEXEC: %s", strerror(errno));
        else if (d < 100) FAILF("got fd %d, want >= 100", d);
        else {
            int fl = fcntl(d, F_GETFD);
            if (fl >= 0 && (fl & FD_CLOEXEC)) PASS();
            else FAILF("F_GETFD = %d, want CLOEXEC set", fl);
            close(d);
        }
    }

    TEST("F_GETFL reports stdout as writable, not O_RDONLY");
    {
        int fl = fcntl(1, F_GETFL);
        if (fl < 0) FAILF("F_GETFL: %s", strerror(errno));
        else if ((fl & O_ACCMODE) == O_WRONLY || (fl & O_ACCMODE) == O_RDWR)
            PASS();
        else FAILF("access mode %d (O_RDONLY) — status flags never seeded",
                   fl & O_ACCMODE);
    }

    /* The real point of F_SETFL: it must change how the fd behaves. */
    TEST("F_SETFL(O_NONBLOCK) actually makes read() non-blocking");
    {
        if (fcntl(p[0], F_SETFL, O_NONBLOCK) != 0) {
            FAILF("F_SETFL: %s", strerror(errno));
        } else {
            char c;
            errno = 0;
            ssize_t n = read(p[0], &c, 1);   /* pipe is empty */
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) PASS();
            else FAILF("read returned %zd (%s) — O_NONBLOCK never reached "
                       "the descriptor", n, strerror(errno));
        }
    }

    TEST("F_GETFL reads back the O_NONBLOCK that was set");
    {
        int fl = fcntl(p[0], F_GETFL);
        if (fl >= 0 && (fl & O_NONBLOCK)) PASS();
        else FAILF("F_GETFL = %d, want O_NONBLOCK set", fl);
    }
    close(p[0]);
    close(p[1]);

    /* (3) dup3 over a live eventfd must retire the eventfd's state. */
    TEST("an eventfd reusing a dup3'd-over number starts fresh");
    {
        int e = eventfd(5, 0);   /* counter 5, self-pipe primed */
        if (e < 0) {
            FAILF("setup: %s", strerror(errno));
        } else {
            /* Source must be DESCRIPTOR-backed (fd 1): that is the branch
             * routed through hl_fd_dup3, which pre-removes the target and
             * so never reaches fd_alloc_at's special-fd cleanup. With a
             * plain pipe as the source, sys_dup3 takes the legacy path and
             * fd_alloc_at retires the eventfd correctly. */
            if (dup3(1, e, 0) != e) {
                FAILF("dup3: %s", strerror(errno));
            } else {
                close(e);

                int e2 = eventfd(0, EFD_NONBLOCK);
                if (e2 < 0) {
                    FAILF("eventfd: %s", strerror(errno));
                } else if (e2 != e) {
                    /* Not the reuse case; the test would prove nothing. */
                    FAILF("expected fd %d to be reused, got %d", e, e2);
                    close(e2);
                } else {
                    /* (-) The leaked slot still answers to this fd number,
                     * so eventfd_find() matches it first: the "new" eventfd
                     * reports the old counter and is readable at once. */
                    uint64_t got = 0;
                    errno = 0;
                    ssize_t r = read(e2, &got, sizeof(got));
                    if (r > 0)
                        FAILF("fresh eventfd(0) returned %llu — inherited "
                              "the stale slot", (unsigned long long)got);
                    else if (errno != EAGAIN && errno != EWOULDBLOCK)
                        FAILF("read: %zd (%s), want EAGAIN",
                              r, strerror(errno));
                    else {
                        /* (+) and it must still work as an eventfd. */
                        uint64_t v = 7;
                        ssize_t w = write(e2, &v, sizeof(v));
                        struct pollfd pfd = { .fd = e2, .events = POLLIN };
                        int pr = poll(&pfd, 1, 1000);
                        got = 0;
                        r = (pr > 0) ? read(e2, &got, sizeof(got)) : -1;
                        if (w != (ssize_t)sizeof(v))
                            FAILF("write: %zd (%s)", w, strerror(errno));
                        else if (pr <= 0)
                            FAIL("poll never became ready");
                        else if (r != (ssize_t)sizeof(got) || got != 7)
                            FAILF("read %zd bytes, value %llu, want 7",
                                  r, (unsigned long long)got);
                        else PASS();
                    }
                    close(e2);
                }
            }
        }
    }

    SUMMARY("test-fcntl-dup");
    return fails ? 1 : 0;
}
