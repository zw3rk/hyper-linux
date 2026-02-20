/* test-eventfd.c — Test eventfd2 syscall emulation
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: eventfd create with flags, counter read/write semantics,
 *        EFD_SEMAPHORE mode, EFD_NONBLOCK EAGAIN, poll readiness
 *
 * Syscalls exercised: eventfd2(19), read(63), write(64), poll/ppoll(73),
 *                     close(57)
 */
#include "test-harness.h"
#include <stdint.h>
#include <unistd.h>
#include <poll.h>
#include <sys/eventfd.h>

int main(void) {
    int passes = 0, fails = 0;

    printf("test-eventfd: eventfd2 emulation tests\n");

    /* Test eventfd create with CLOEXEC + NONBLOCK flags */
    TEST("eventfd(EFD_CLOEXEC|NONBLOCK)");
    {
        int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (fd >= 0) PASS();
        else FAIL("eventfd create failed");
        close(fd);
    }

    /* Test write + read counter semantics (non-semaphore) */
    TEST("write+read counter");
    {
        int fd = eventfd(0, EFD_NONBLOCK);
        if (fd >= 0) {
            uint64_t val = 5;
            ssize_t w = write(fd, &val, sizeof(val));
            if (w == 8) {
                uint64_t out = 0;
                ssize_t r = read(fd, &out, sizeof(out));
                if (r == 8 && out == 5) PASS();
                else FAIL("counter mismatch");
            } else FAIL("write failed");
            close(fd);
        } else FAIL("eventfd create failed");
    }

    /* Test counter accumulation: two writes, one read returns sum */
    TEST("counter accumulation");
    {
        int fd = eventfd(0, EFD_NONBLOCK);
        if (fd >= 0) {
            uint64_t v1 = 3, v2 = 7;
            write(fd, &v1, sizeof(v1));
            write(fd, &v2, sizeof(v2));
            uint64_t out = 0;
            ssize_t r = read(fd, &out, sizeof(out));
            if (r == 8 && out == 10) PASS();
            else FAIL("accumulation wrong");
            close(fd);
        } else FAIL("eventfd create failed");
    }

    /* Test EFD_SEMAPHORE: write 3, read returns 1 three times, then EAGAIN */
    TEST("EFD_SEMAPHORE");
    {
        int fd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
        if (fd >= 0) {
            uint64_t val = 3;
            write(fd, &val, sizeof(val));
            int ok = 1;
            for (int i = 0; i < 3; i++) {
                uint64_t out = 0;
                ssize_t r = read(fd, &out, sizeof(out));
                if (r != 8 || out != 1) { ok = 0; break; }
            }
            /* Fourth read should fail with EAGAIN */
            uint64_t out = 0;
            ssize_t r = read(fd, &out, sizeof(out));
            if (ok && r == -1 && errno == EAGAIN) PASS();
            else FAIL("semaphore semantics wrong");
            close(fd);
        } else FAIL("eventfd create failed");
    }

    /* Test EAGAIN on empty eventfd (nonblocking) */
    TEST("EAGAIN on empty");
    {
        int fd = eventfd(0, EFD_NONBLOCK);
        if (fd >= 0) {
            uint64_t out = 0;
            ssize_t r = read(fd, &out, sizeof(out));
            if (r == -1 && errno == EAGAIN) PASS();
            else FAIL("expected EAGAIN");
            close(fd);
        } else FAIL("eventfd create failed");
    }

    /* Test poll: POLLIN should be set after write */
    TEST("poll(POLLIN after write)");
    {
        int fd = eventfd(0, EFD_NONBLOCK);
        if (fd >= 0) {
            uint64_t val = 1;
            write(fd, &val, sizeof(val));
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int ret = poll(&pfd, 1, 0);
            if (ret > 0 && (pfd.revents & POLLIN)) PASS();
            else FAIL("poll did not report POLLIN");
            close(fd);
        } else FAIL("eventfd create failed");
    }

    /* Test initial value is readable immediately */
    TEST("eventfd(initval=42)");
    {
        int fd = eventfd(42, EFD_NONBLOCK);
        if (fd >= 0) {
            uint64_t out = 0;
            ssize_t r = read(fd, &out, sizeof(out));
            if (r == 8 && out == 42) PASS();
            else FAIL("initial value wrong");
            close(fd);
        } else FAIL("eventfd create failed");
    }

    printf("\nResults: %d passed, %d failed\n", passes, fails);
    return fails > 0 ? 1 : 0;
}
