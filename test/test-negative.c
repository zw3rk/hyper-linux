/* test-negative.c — Negative / error-path tests for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Exercises error paths: invalid FDs, bad syscall numbers, bad mmap
 * arguments, EFAULT (bad pointers), errno translation, EINVAL (bad flags),
 * and boundary conditions. All should return proper Linux error codes
 * without crashing.
 */
#include "test-harness.h"
#include "raw-syscall.h"
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/syscall.h>
#include <time.h>

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

/* ---------- Test 10: EFAULT — bad pointer to read/write/getcwd ---------- */

static void test_efault(void) {
    /* Use an unmapped high address as a bad pointer.
     * Raw syscalls return -errno directly. */
    void *bad_ptr = (void *)0xDEAD000000000000ULL;

    TEST("read(0, bad_ptr) → EFAULT");
    {
        /* Linux EFAULT = 14 */
        long r = raw_syscall3(__NR_read, 0, (long)bad_ptr, 16);
        if (r == -14) PASS();
        else FAIL("expected -EFAULT (-14)");
    }

    TEST("write(1, bad_ptr) → EFAULT");
    {
        long r = raw_syscall3(__NR_write, 1, (long)bad_ptr, 16);
        if (r == -14) PASS();
        else FAIL("expected -EFAULT (-14)");
    }

    TEST("getcwd(bad_ptr) → EFAULT");
    {
        long r = raw_syscall2(__NR_getcwd, (long)bad_ptr, 256);
        if (r == -14) PASS();
        else FAIL("expected -EFAULT (-14)");
    }

    TEST("uname(bad_ptr) → EFAULT");
    {
        long r = raw_syscall1(__NR_uname, (long)bad_ptr);
        if (r == -14) PASS();
        else FAIL("expected -EFAULT (-14)");
    }

    TEST("clock_gettime(bad_ptr) → EFAULT");
    {
        long r = raw_syscall2(__NR_clock_gettime, 0 /* CLOCK_REALTIME */,
                              (long)bad_ptr);
        if (r == -14) PASS();
        else FAIL("expected -EFAULT (-14)");
    }
}

/* ---------- Test 11: Linux errno numeric values ---------- */

static void test_errno_values(void) {
    /* Verify that hl translates macOS errno → Linux errno correctly.
     * These are the most commonly divergent values. */

    TEST("EAGAIN is 11");
    {
        /* Non-blocking read on empty pipe gives EAGAIN */
        int pipefd[2];
        if (pipe(pipefd) != 0) { FAIL("pipe() failed"); return; }
        fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
        char buf[1];
        ssize_t r = read(pipefd[0], buf, 1);
        if (r == -1 && errno == 11) PASS();
        else FAIL("expected errno 11 (EAGAIN)");
        close(pipefd[0]);
        close(pipefd[1]);
    }

    TEST("ENOSYS is 38");
    {
        /* Bogus syscall number */
        long r = raw_syscall1(9999, 0);
        if (r == -38) PASS();
        else FAIL("expected -38 (ENOSYS)");
    }

    TEST("EBADF is 9");
    {
        /* read on bad fd */
        long r = raw_syscall3(__NR_read, 999, 0, 0);
        if (r == -9) PASS();
        else FAIL("expected -9 (EBADF)");
    }

    TEST("EINVAL is 22");
    {
        /* clock_gettime with invalid clock ID */
        struct timespec ts;
        long r = raw_syscall2(__NR_clock_gettime, 99 /* invalid */,
                              (long)&ts);
        if (r == -22) PASS();
        else FAIL("expected -22 (EINVAL)");
    }

    TEST("ESPIPE is 29");
    {
        /* lseek on pipe */
        int pipefd[2];
        if (pipe(pipefd) != 0) { FAIL("pipe() failed"); return; }
        long r = raw_syscall3(__NR_lseek, pipefd[0], 0, 0 /* SEEK_SET */);
        if (r == -29) PASS();
        else FAIL("expected -29 (ESPIPE)");
        close(pipefd[0]);
        close(pipefd[1]);
    }
}

/* ---------- Test 12: EINVAL — bad arguments ---------- */

static void test_einval(void) {
    TEST("socket(bad domain) → error");
    {
        /* AF 255 is not a valid address family */
        int s = socket(255, SOCK_STREAM, 0);
        if (s == -1 && (errno == EINVAL ||
                        errno == EAFNOSUPPORT ||
                        errno == EPROTONOSUPPORT)) PASS();
        else {
            if (s >= 0) close(s);
            FAIL("expected EINVAL/EAFNOSUPPORT");
        }
    }

    TEST("munmap(0, 0) → EINVAL");
    {
        int r = munmap(NULL, 0);
        if (r == -1 && errno == EINVAL) PASS();
        else FAIL("expected EINVAL");
    }

    TEST("mmap(size=0) → EINVAL");
    {
        void *p = mmap(NULL, 0, PROT_READ,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED && errno == EINVAL) PASS();
        else {
            if (p != MAP_FAILED) munmap(p, 4096);
            FAIL("expected EINVAL for zero size");
        }
    }
}

/* ---------- Test 13: Socket error paths ---------- */

static void test_socket_errors(void) {
    TEST("connect(bad fd) → EBADF");
    {
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        int r = connect(999, (struct sockaddr *)&addr, sizeof(addr));
        if (r == -1 && errno == EBADF) PASS();
        else FAIL("expected EBADF");
    }

    TEST("getsockopt(bad fd) → EBADF");
    {
        int val;
        socklen_t len = sizeof(val);
        int r = getsockopt(999, SOL_SOCKET, SO_TYPE, &val, &len);
        if (r == -1 && errno == EBADF) PASS();
        else FAIL("expected EBADF");
    }

    TEST("accept(non-socket) → ENOTSOCK");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) { FAIL("pipe failed"); return; }
        int r = accept(pipefd[0], NULL, NULL);
        if (r == -1 && errno == ENOTSOCK) PASS();
        else FAIL("expected ENOTSOCK");
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
    test_efault();
    test_errno_values();
    test_einval();
    test_socket_errors();

    SUMMARY("test-negative");
    return fails > 0 ? 1 : 0;
}
