/* test-negative.c — Negative / error-path tests for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Exercises error paths: invalid FDs, bad syscall numbers, bad mmap
 * arguments, and boundary conditions. All should return proper Linux
 * error codes without crashing.
 *
 * Syscalls exercised: read(63), write(64), close(57), dup(23),
 *                     mmap(222), munmap(215), mprotect(226)
 */
#include "test-harness.h"
#include "raw-syscall.h"
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>

int passes = 0, fails = 0;

/* ---------- Test 1: Invalid FD operations ---------- */

static void test_invalid_fd(void) {
    TEST("read(999) → EBADF");
    {
        char buf[16];
        ssize_t r = read(999, buf, sizeof(buf));
        if (r == -1 && errno == EBADF) PASS();
        else FAIL("expected EBADF");
    }

    TEST("write(999) → EBADF");
    {
        ssize_t r = write(999, "x", 1);
        if (r == -1 && errno == EBADF) PASS();
        else FAIL("expected EBADF");
    }

    TEST("close(999) → EBADF");
    {
        int r = close(999);
        if (r == -1 && errno == EBADF) PASS();
        else FAIL("expected EBADF");
    }

    TEST("dup(-1) → EBADF");
    {
        int r = dup(-1);
        if (r == -1 && errno == EBADF) PASS();
        else FAIL("expected EBADF");
    }
}

/* ---------- Test 2: Invalid syscall number → ENOSYS ---------- */

static void test_invalid_syscall(void) {
    TEST("syscall(9999) → ENOSYS");
    {
        /* Use raw syscall with an absurdly high number */
        long r = raw_syscall1(9999, 0);
        /* Linux returns -ENOSYS = -38 */
        if (r == -38) PASS();
        else FAIL("expected -ENOSYS (-38)");
    }
}

/* ---------- Test 3: mmap with bad arguments ---------- */

static void test_bad_mmap(void) {
    TEST("mmap bad fd (not ANON) → error");
    {
        /* Non-anonymous mmap with invalid fd should fail */
        void *p = mmap(NULL, 4096, PROT_READ,
                        MAP_PRIVATE, 999, 0);
        if (p == MAP_FAILED) PASS();
        else { munmap(p, 4096); FAIL("expected MAP_FAILED"); }
    }

    TEST("mmap(MAP_FIXED, bad addr) → error");
    {
        /* MAP_FIXED at an absurdly high address should fail */
        void *p = mmap((void *)0xFFFF000000000000ULL, 4096,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                        -1, 0);
        if (p == MAP_FAILED) PASS();
        else FAIL("expected MAP_FAILED");
    }

    TEST("mmap + munmap round-trip");
    {
        /* Verify basic mmap+munmap lifecycle works */
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            FAIL("mmap failed");
        } else {
            *(volatile int *)p = 42;
            int r = munmap(p, 4096);
            if (r == 0) PASS();
            else FAIL("munmap failed");
        }
    }
}

/* ---------- Test 4: Read/write on closed FD ---------- */

static void test_closed_fd(void) {
    TEST("read closed pipe → EBADF");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            FAIL("pipe() failed");
            return;
        }
        int rd = pipefd[0];
        close(rd);

        char buf[16];
        ssize_t r = read(rd, buf, sizeof(buf));
        if (r == -1 && errno == EBADF) PASS();
        else FAIL("expected EBADF on closed FD");

        close(pipefd[1]);
    }
}

/* ---------- Test 5: Double close ---------- */

static void test_double_close(void) {
    TEST("double close → EBADF");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            FAIL("pipe() failed");
            return;
        }
        close(pipefd[0]);
        int r = close(pipefd[0]); /* Second close should fail */
        if (r == -1 && errno == EBADF) PASS();
        else FAIL("expected EBADF on double close");

        close(pipefd[1]);
    }
}

/* ---------- Test 6: Write to read-only mmap region ---------- */

static void test_mmap_prot(void) {
    TEST("mmap read-only is readable");
    {
        /* Allocate RW, write data, change to RO, verify readable */
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            FAIL("mmap failed");
            return;
        }
        *(volatile int *)p = 42;
        mprotect(p, 4096, PROT_READ);
        int val = *(volatile int *)p;
        if (val == 42) PASS();
        else FAIL("data not readable after RO mprotect");
        munmap(p, 4096);
    }
}

/* ---------- Test 7: fcntl on invalid FD ---------- */

static void test_fcntl_invalid(void) {
    TEST("fcntl(999, F_GETFL) → EBADF");
    {
        int r = fcntl(999, F_GETFL);
        if (r == -1 && errno == EBADF) PASS();
        else FAIL("expected EBADF");
    }
}

/* ---------- Test 8: lseek on pipe → ESPIPE ---------- */

static void test_lseek_pipe(void) {
    TEST("lseek(pipe) → ESPIPE");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            FAIL("pipe() failed");
            return;
        }
        off_t r = lseek(pipefd[0], 0, SEEK_SET);
        if (r == (off_t)-1 && errno == ESPIPE) PASS();
        else FAIL("expected ESPIPE");
        close(pipefd[0]);
        close(pipefd[1]);
    }
}

/* ---------- Test 9: Read from write-only FD ---------- */

static void test_write_only_read(void) {
    TEST("read(write-end of pipe) → EBADF");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            FAIL("pipe() failed");
            return;
        }
        char buf[16];
        ssize_t r = read(pipefd[1], buf, sizeof(buf));
        if (r == -1 && errno == EBADF) PASS();
        else FAIL("expected EBADF");
        close(pipefd[0]);
        close(pipefd[1]);
    }
}

/* ---------- Main ---------- */

int main(void) {
    printf("test-negative: error path tests\n");

    test_invalid_fd();
    test_invalid_syscall();
    test_bad_mmap();
    test_closed_fd();
    test_double_close();
    test_mmap_prot();
    test_fcntl_invalid();
    test_lseek_pipe();
    test_write_only_read();

    printf("\ntest-negative: %d passed, %d failed\n", passes, fails);
    return fails > 0 ? 1 : 0;
}
