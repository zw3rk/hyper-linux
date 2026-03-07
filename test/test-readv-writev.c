/* test-readv-writev.c — Scatter-gather I/O tests
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests readv/writev scatter-gather I/O operations including
 * pipe round-trips, file I/O, and edge cases.
 *
 * Syscalls exercised: writev(66), readv(65), pipe2(59), openat(56),
 *                     lseek(62), close(57), unlink(35)
 */
#include "test-harness.h"
#include <fcntl.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

int passes = 0, fails = 0;

/* ---------- Test 1: writev + readv via pipe ---------- */

static void test_pipe_roundtrip(void) {
    TEST("writev→readv pipe roundtrip");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) { FAIL("pipe"); return; }

        /* Write 3 iovecs: "hello", " ", "world" */
        struct iovec wv[3] = {
            { .iov_base = (void *)"hello", .iov_len = 5 },
            { .iov_base = (void *)" ",     .iov_len = 1 },
            { .iov_base = (void *)"world", .iov_len = 5 },
        };
        ssize_t nw = writev(pipefd[1], wv, 3);
        if (nw != 11) { FAIL("writev returned wrong count"); close(pipefd[0]); close(pipefd[1]); return; }

        /* Read into 2 iovecs: 6 bytes + 5 bytes */
        char buf1[6] = {0}, buf2[5] = {0};
        struct iovec rv[2] = {
            { .iov_base = buf1, .iov_len = 6 },
            { .iov_base = buf2, .iov_len = 5 },
        };
        ssize_t nr = readv(pipefd[0], rv, 2);
        close(pipefd[0]);
        close(pipefd[1]);

        if (nr != 11) { FAIL("readv returned wrong count"); return; }
        if (memcmp(buf1, "hello ", 6) == 0 && memcmp(buf2, "world", 5) == 0)
            PASS();
        else
            FAIL("data mismatch");
    }
}

/* ---------- Test 2: writev to file + readv back ---------- */

static void test_file_roundtrip(void) {
    TEST("writev→readv file roundtrip");
    {
        const char *path = "/tmp/hl-test-writev.txt";
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { FAIL("open"); return; }

        /* Write structured data */
        int num = 42;
        char str[] = "test-data";
        struct iovec wv[2] = {
            { .iov_base = &num,  .iov_len = sizeof(num) },
            { .iov_base = str,   .iov_len = sizeof(str) },
        };
        ssize_t nw = writev(fd, wv, 2);
        if (nw != (ssize_t)(sizeof(num) + sizeof(str))) {
            FAIL("writev wrong count");
            close(fd);
            unlink(path);
            return;
        }

        /* Seek back and read */
        lseek(fd, 0, SEEK_SET);

        int num2 = 0;
        char str2[sizeof(str)];
        memset(str2, 0, sizeof(str2));
        struct iovec rv[2] = {
            { .iov_base = &num2,  .iov_len = sizeof(num2) },
            { .iov_base = str2,   .iov_len = sizeof(str2) },
        };
        ssize_t nr = readv(fd, rv, 2);
        close(fd);
        unlink(path);

        if (nr != nw) { FAIL("readv wrong count"); return; }
        if (num2 == 42 && strcmp(str2, "test-data") == 0) PASS();
        else FAIL("data mismatch");
    }
}

/* ---------- Test 3: Single iovec (degenerate case) ---------- */

static void test_single_iovec(void) {
    TEST("writev single iovec");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) { FAIL("pipe"); return; }

        char msg[] = "single";
        struct iovec v = { .iov_base = msg, .iov_len = 6 };
        ssize_t nw = writev(pipefd[1], &v, 1);

        char buf[6] = {0};
        struct iovec rv = { .iov_base = buf, .iov_len = 6 };
        ssize_t nr = readv(pipefd[0], &rv, 1);
        close(pipefd[0]);
        close(pipefd[1]);

        if (nw == 6 && nr == 6 && memcmp(buf, "single", 6) == 0) PASS();
        else FAIL("single iovec mismatch");
    }
}

/* ---------- Test 4: Zero-length iovec ---------- */

static void test_zero_length_iovec(void) {
    TEST("writev with zero-length iovec");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) { FAIL("pipe"); return; }

        struct iovec wv[3] = {
            { .iov_base = (void *)"a",  .iov_len = 1 },
            { .iov_base = (void *)"",   .iov_len = 0 },  /* zero length */
            { .iov_base = (void *)"b",  .iov_len = 1 },
        };
        ssize_t nw = writev(pipefd[1], wv, 3);

        char buf[2] = {0};
        ssize_t nr = read(pipefd[0], buf, 2);
        close(pipefd[0]);
        close(pipefd[1]);

        if (nw == 2 && nr == 2 && buf[0] == 'a' && buf[1] == 'b') PASS();
        else FAIL("zero-length iovec handling wrong");
    }
}

/* ---------- Test 5: Large writev (many iovecs) ---------- */

static void test_many_iovecs(void) {
    TEST("writev 10 iovecs");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) { FAIL("pipe"); return; }

        char data[10] = "0123456789";
        struct iovec wv[10];
        for (int i = 0; i < 10; i++) {
            wv[i].iov_base = &data[i];
            wv[i].iov_len  = 1;
        }
        ssize_t nw = writev(pipefd[1], wv, 10);

        char buf[10] = {0};
        ssize_t nr = read(pipefd[0], buf, 10);
        close(pipefd[0]);
        close(pipefd[1]);

        if (nw == 10 && nr == 10 && memcmp(buf, "0123456789", 10) == 0) PASS();
        else FAIL("10-iovec writev failed");
    }
}

/* ---------- Main ---------- */

int main(void) {
    printf("test-readv-writev: scatter-gather I/O tests\n");

    test_pipe_roundtrip();
    test_file_roundtrip();
    test_single_iovec();
    test_zero_length_iovec();
    test_many_iovecs();

    printf("\ntest-readv-writev: %d passed, %d failed\n", passes, fails);
    return fails > 0 ? 1 : 0;
}
