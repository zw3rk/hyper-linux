/* test-poll.c — Test signals + I/O multiplexing syscalls (Batch 4)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: ppoll, pselect, kill, signal ops
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>
#include <errno.h>

#define TEST(name) printf("  %-30s ", name)
#define PASS()     do { printf("OK\n"); passes++; } while(0)
#define FAIL(msg)  do { printf("FAIL: %s (errno=%d)\n", msg, errno); fails++; } while(0)

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

    /* Test pselect with timeout */
    TEST("pselect (timeout)");
    {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 0 };
        int ret = pselect(0, NULL, NULL, NULL, &ts, NULL);
        if (ret == 0) PASS();  /* No fds, immediate timeout */
        else FAIL("pselect failed");
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

    printf("\nResults: %d passed, %d failed\n", passes, fails);
    return fails > 0 ? 1 : 0;
}
