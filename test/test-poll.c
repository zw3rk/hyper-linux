/* test-poll.c — Test signals + I/O multiplexing syscalls (Batch 4)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: ppoll, pselect, kill, signal ops
 */
#include "test-harness.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>

int main(void) {
    int passes = 0, fails = 0;

    printf("test-poll: Batch 4 signals + I/O multiplexing tests\n");

    /* Test ppoll with pipe (should be ready for write) */
    TEST("ppoll (pipe write-ready)");
    {
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            struct pollfd fds[1];
            fds[0].fd = pipefd[1];  /* write end */
            fds[0].events = POLLOUT;
            fds[0].revents = 0;

            struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
            int ret = ppoll(fds, 1, &ts, NULL);
            if (ret >= 0 && (fds[0].revents & POLLOUT)) PASS();
            else FAIL("pipe not writable");
            close(pipefd[0]);
            close(pipefd[1]);
        } else FAIL("pipe failed");
    }

    /* Test ppoll with timeout (0 = immediate return) */
    TEST("ppoll (timeout)");
    {
        struct pollfd fds[1];
        fds[0].fd = 0;  /* stdin */
        fds[0].events = POLLIN;
        fds[0].revents = 0;

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
        int ret = ppoll(fds, 1, &ts, NULL);
        if (ret >= 0) PASS();  /* 0 = no data ready, which is expected */
        else FAIL("ppoll failed");
    }

    /* Sub-millisecond positive timeout must wait, not busy as timeout=0.
     * hl clamps each such wait to ≥1ms; 20 iterations should take ≥10ms. */
    TEST("ppoll (sub-ms timeout)");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            FAIL("pipe failed");
        } else {
            struct pollfd fds[1];
            fds[0].fd = pipefd[0]; /* read end — not ready */
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000 }; /* 0.5 ms */
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int ok = 1;
            for (int i = 0; i < 20; i++) {
                fds[0].revents = 0;
                if (ppoll(fds, 1, &ts, NULL) < 0)
                    ok = 0;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long long dt_ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
                              (t1.tv_nsec - t0.tv_nsec);
            close(pipefd[0]);
            close(pipefd[1]);
            /* 20 × ~1ms clamp → expect ≥10ms wall */
            if (ok && dt_ns >= 10000000LL) PASS();
            else FAIL("sub-ms ppoll busy-spun or failed");
        }
    }

    /* Closed nonnegative fd → POLLNVAL (timeout=0 path) */
    TEST("ppoll (POLLNVAL on closed fd)");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            FAIL("pipe failed");
        } else {
            int bad = pipefd[0];
            close(pipefd[0]);
            close(pipefd[1]);
            struct pollfd fds[1];
            fds[0].fd = bad;
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
            int ret = ppoll(fds, 1, &ts, NULL);
            if (ret >= 1 && (fds[0].revents & POLLNVAL)) PASS();
            else FAIL("expected POLLNVAL");
        }
    }

    /* Closed fd + long positive timeout must return immediately with POLLNVAL
     * (Linux does not wait — invalid fd is already "ready"). */
    TEST("ppoll (POLLNVAL + 1s timeout, no hang)");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            FAIL("pipe failed");
        } else {
            int bad = pipefd[0];
            close(pipefd[0]);
            close(pipefd[1]);
            struct pollfd fds[1];
            fds[0].fd = bad;
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int ret = ppoll(fds, 1, &ts, NULL);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long long dt_ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
                              (t1.tv_nsec - t0.tv_nsec);
            /* Must not consume the 1s timeout; allow a few ms of overhead. */
            if (ret >= 1 && (fds[0].revents & POLLNVAL) && dt_ns < 200000000LL)
                PASS();
            else
                FAIL("POLLNVAL waited or missing");
        }
    }

    /* Closed fd + infinite timeout (NULL) must not hang. */
    TEST("ppoll (POLLNVAL + infinite timeout, no hang)");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            FAIL("pipe failed");
        } else {
            int bad = pipefd[0];
            close(pipefd[0]);
            close(pipefd[1]);
            struct pollfd fds[1];
            fds[0].fd = bad;
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int ret = ppoll(fds, 1, NULL, NULL);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long long dt_ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
                              (t1.tv_nsec - t0.tv_nsec);
            if (ret >= 1 && (fds[0].revents & POLLNVAL) && dt_ns < 200000000LL)
                PASS();
            else
                FAIL("infinite POLLNVAL hang or missing");
        }
    }

    /* Test pselect with timeout */
    TEST("pselect (timeout)");
    {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
        int ret = pselect(0, NULL, NULL, NULL, &ts, NULL);
        if (ret == 0) PASS();  /* No fds, immediate timeout */
        else FAIL("pselect failed");
    }

    /* Closed fd in pselect set → EBADF (Linux select semantics). */
    TEST("pselect (EBADF on closed fd)");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            FAIL("pipe failed");
        } else {
            int bad = pipefd[0];
            close(pipefd[0]);
            close(pipefd[1]);
            fd_set rset;
            FD_ZERO(&rset);
            FD_SET(bad, &rset);
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            struct timespec t0, t1;
            errno = 0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int ret = pselect(bad + 1, &rset, NULL, NULL, &ts, NULL);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long long dt_ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
                              (t1.tv_nsec - t0.tv_nsec);
            if (ret < 0 && errno == EBADF && dt_ns < 200000000LL)
                PASS();
            else
                FAIL("expected EBADF quickly");
        }
    }

    /* Test kill(getpid(), 0) — process existence check */
    TEST("kill(getpid, 0)");
    {
        if (kill(getpid(), 0) == 0) PASS();
        else FAIL("kill existence check failed");
    }

    /* Test signal mask operations (should not crash) */
    TEST("sigprocmask");
    {
        sigset_t set, oldset;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        if (sigprocmask(SIG_BLOCK, &set, &oldset) == 0) PASS();
        else FAIL("sigprocmask failed");
    }

    /* Test sigaction (should succeed as stub) */
    TEST("sigaction");
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        if (sigaction(SIGUSR1, &sa, NULL) == 0) PASS();
        else FAIL("sigaction failed");
    }

    /* Test setsid / setpgid stubs */
    TEST("setpgid");
    {
        /* Should return 0 (stub) */
        if (setpgid(0, 0) == 0) PASS();
        else FAIL("setpgid failed");
    }

    SUMMARY("test-poll");
    return fails > 0 ? 1 : 0;
}
