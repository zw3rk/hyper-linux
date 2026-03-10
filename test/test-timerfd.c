/* test-timerfd.c — Test timerfd emulation (kqueue backend)
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: timerfd_create, timerfd_settime (one-shot + interval),
 *        timerfd_gettime, read expiration count, poll readiness
 *
 * Syscalls exercised: timerfd_create(85), timerfd_settime(86),
 *                     timerfd_gettime(87), read(63), ppoll(73), close(57),
 *                     nanosleep(101)
 */
#include "test-harness.h"
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <sys/timerfd.h>

int main(void) {
    int passes = 0, fails = 0;

    printf("test-timerfd: timerfd emulation tests\n");

    /* Test timerfd_create */
    TEST("timerfd_create");
    {
        int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (fd >= 0) PASS();
        else FAIL("timerfd_create failed");
        close(fd);
    }

    /* Test one-shot timer: 20ms, then read expiration count */
    TEST("one-shot 20ms timer");
    {
        int fd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (fd >= 0) {
            struct itimerspec its = {
                .it_value = { .tv_sec = 0, .tv_nsec = 20000000 }, /* 20ms */
                .it_interval = { 0, 0 }
            };
            if (timerfd_settime(fd, 0, &its, NULL) == 0) {
                usleep(50000); /* 50ms — plenty of time */
                uint64_t count = 0;
                ssize_t r = read(fd, &count, sizeof(count));
                if (r == 8 && count >= 1) PASS();
                else FAIL("read failed or count=0");
            } else FAIL("timerfd_settime failed");
            close(fd);
        } else FAIL("timerfd_create failed");
    }

    /* Test timerfd_gettime: one-shot interval should be 0 after expiry */
    TEST("timerfd_gettime after expiry");
    {
        int fd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (fd >= 0) {
            struct itimerspec its = {
                .it_value = { .tv_sec = 0, .tv_nsec = 10000000 }, /* 10ms */
                .it_interval = { 0, 0 }
            };
            timerfd_settime(fd, 0, &its, NULL);
            usleep(30000); /* 30ms */

            /* Drain the expiration */
            uint64_t count;
            read(fd, &count, sizeof(count));

            struct itimerspec cur;
            if (timerfd_gettime(fd, &cur) == 0) {
                if (cur.it_interval.tv_sec == 0 && cur.it_interval.tv_nsec == 0)
                    PASS();
                else
                    FAIL("interval not zero for one-shot");
            } else FAIL("timerfd_gettime failed");
            close(fd);
        } else FAIL("timerfd_create failed");
    }

    /* Test interval timer: 20ms interval, wait ~90ms, expect 3-5 fires */
    TEST("interval timer (20ms x~4)");
    {
        int fd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (fd >= 0) {
            struct itimerspec its = {
                .it_value    = { .tv_sec = 0, .tv_nsec = 20000000 }, /* 20ms */
                .it_interval = { .tv_sec = 0, .tv_nsec = 20000000 }  /* 20ms */
            };
            if (timerfd_settime(fd, 0, &its, NULL) == 0) {
                usleep(90000); /* 90ms */
                uint64_t count = 0;
                ssize_t r = read(fd, &count, sizeof(count));
                if (r == 8 && count >= 2) PASS();
                else FAIL("interval count wrong");
            } else FAIL("timerfd_settime failed");
            close(fd);
        } else FAIL("timerfd_create failed");
    }

    /* Test poll on timerfd: should report POLLIN after timer fires */
    TEST("poll on timerfd");
    {
        int fd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (fd >= 0) {
            struct itimerspec its = {
                .it_value = { .tv_sec = 0, .tv_nsec = 10000000 }, /* 10ms */
                .it_interval = { 0, 0 }
            };
            timerfd_settime(fd, 0, &its, NULL);

            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int ret = poll(&pfd, 1, 200); /* 200ms timeout */
            if (ret > 0 && (pfd.revents & POLLIN)) PASS();
            else FAIL("poll did not report POLLIN");
            close(fd);
        } else FAIL("timerfd_create failed");
    }

    SUMMARY("test-timerfd");
    return fails > 0 ? 1 : 0;
}
