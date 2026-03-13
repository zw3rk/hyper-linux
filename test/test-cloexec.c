/* test-cloexec.c — Test O_CLOEXEC behavior across fork and exec
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies:
 * 1. pipe2(O_CLOEXEC) sets FD_CLOEXEC on both pipe FDs
 * 2. Plain pipe() does NOT set FD_CLOEXEC
 * 3. fcntl(F_SETFD, FD_CLOEXEC) sets the flag correctly
 * 4. dup() clears CLOEXEC on the new FD
 * 5. dup3(O_CLOEXEC) sets CLOEXEC on the new FD
 * 6. Non-CLOEXEC FDs survive fork
 * 7. CLOEXEC FDs ARE inherited across fork (POSIX: CLOEXEC only at exec)
 *
 * Syscalls exercised: pipe2(59), fcntl(25), dup(23), dup3(24),
 *                     fork/clone, close(57), read(63), write(64)
 */
#include "test-harness.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <string.h>

int passes = 0, fails = 0;

/* ---------- Test 1: pipe2(O_CLOEXEC) sets FD_CLOEXEC ---------- */

static void test_pipe2_cloexec(void) {
    TEST("pipe2(O_CLOEXEC) sets flag");

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        FAIL("pipe2() failed");
        return;
    }

    int flags0 = fcntl(pipefd[0], F_GETFD);
    int flags1 = fcntl(pipefd[1], F_GETFD);

    if ((flags0 & FD_CLOEXEC) && (flags1 & FD_CLOEXEC))
        PASS();
    else
        FAIL("FD_CLOEXEC not set on pipe FDs");

    close(pipefd[0]);
    close(pipefd[1]);
}

/* ---------- Test 2: Plain pipe does NOT set FD_CLOEXEC ---------- */

static void test_pipe_no_cloexec(void) {
    TEST("pipe() no CLOEXEC");

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        FAIL("pipe() failed");
        return;
    }

    int flags0 = fcntl(pipefd[0], F_GETFD);
    int flags1 = fcntl(pipefd[1], F_GETFD);

    if (!(flags0 & FD_CLOEXEC) && !(flags1 & FD_CLOEXEC))
        PASS();
    else
        FAIL("FD_CLOEXEC unexpectedly set");

    close(pipefd[0]);
    close(pipefd[1]);
}

/* ---------- Test 3: fcntl(F_SETFD, FD_CLOEXEC) ---------- */

static void test_fcntl_setfd(void) {
    TEST("fcntl(F_SETFD, FD_CLOEXEC)");

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        FAIL("pipe() failed");
        return;
    }

    /* Set CLOEXEC via fcntl */
    if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) != 0) {
        FAIL("F_SETFD failed");
        close(pipefd[0]); close(pipefd[1]);
        return;
    }

    int flags = fcntl(pipefd[0], F_GETFD);
    if (flags & FD_CLOEXEC)
        PASS();
    else
        FAIL("FD_CLOEXEC not set after F_SETFD");

    close(pipefd[0]);
    close(pipefd[1]);
}

/* ---------- Test 4: dup() clears CLOEXEC ---------- */

static void test_dup_clears_cloexec(void) {
    TEST("dup() clears CLOEXEC");

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        FAIL("pipe2() failed");
        return;
    }

    int newfd = dup(pipefd[0]);
    if (newfd < 0) {
        FAIL("dup() failed");
        close(pipefd[0]); close(pipefd[1]);
        return;
    }

    int flags = fcntl(newfd, F_GETFD);
    if (!(flags & FD_CLOEXEC))
        PASS();
    else
        FAIL("dup'd FD still has CLOEXEC");

    close(newfd);
    close(pipefd[0]);
    close(pipefd[1]);
}

/* ---------- Test 5: dup3(O_CLOEXEC) sets CLOEXEC ---------- */

static void test_dup3_cloexec(void) {
    TEST("dup3(O_CLOEXEC) sets flag");

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        FAIL("pipe() failed");
        return;
    }

    /* Allocate a target FD */
    int target_pipe[2];
    if (pipe(target_pipe) != 0) {
        FAIL("pipe() for target failed");
        close(pipefd[0]); close(pipefd[1]);
        return;
    }
    int target_fd = target_pipe[0];
    close(target_pipe[1]);

    /* dup3 with O_CLOEXEC: dup pipefd[0] onto target_fd */
    int ret = dup3(pipefd[0], target_fd, O_CLOEXEC);
    if (ret < 0) {
        FAIL("dup3() failed");
        close(pipefd[0]); close(pipefd[1]); close(target_fd);
        return;
    }

    int flags = fcntl(target_fd, F_GETFD);
    if (flags & FD_CLOEXEC)
        PASS();
    else
        FAIL("dup3 did not set CLOEXEC");

    close(target_fd);
    close(pipefd[0]);
    close(pipefd[1]);
}

/* ---------- Test 6: Non-CLOEXEC FDs survive fork ---------- */

static void test_no_cloexec_survives_fork(void) {
    TEST("non-CLOEXEC FDs survive fork");

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        FAIL("pipe() failed");
        return;
    }

    int result[2];
    if (pipe(result) != 0) {
        FAIL("pipe() for result failed");
        close(pipefd[0]); close(pipefd[1]);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        FAIL("fork() failed");
        close(pipefd[0]); close(pipefd[1]);
        close(result[0]); close(result[1]);
        return;
    }

    if (pid == 0) {
        /* Child: try writing to the non-CLOEXEC pipe */
        close(result[0]);
        close(pipefd[0]);
        char c;
        if (write(pipefd[1], "z", 1) == 1)
            c = 'Y';
        else
            c = 'N';
        write(result[1], &c, 1);
        close(result[1]);
        close(pipefd[1]);
        _exit(0);
    }

    /* Parent */
    close(result[1]);
    close(pipefd[1]); /* Close write end so read sees EOF if child fails */

    char status_char = 0;
    read(result[0], &status_char, 1);
    close(result[0]);

    char c = 0;
    read(pipefd[0], &c, 1);
    close(pipefd[0]);

    int wstatus;
    waitpid(pid, &wstatus, 0);

    if (status_char == 'Y' && c == 'z')
        PASS();
    else
        FAIL("non-CLOEXEC FD not accessible in fork child");
}

/* ---------- Test 7: CLOEXEC FDs inherited in fork (POSIX) ---------- */

static void test_cloexec_inherited_in_fork(void) {
    TEST("CLOEXEC FDs inherited in fork");

    /* POSIX: CLOEXEC only takes effect at exec, NOT at fork.
     * Fork children inherit all FDs regardless of CLOEXEC. */
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        FAIL("pipe2() failed");
        return;
    }

    int result[2];
    if (pipe(result) != 0) {
        FAIL("pipe() for result failed");
        close(pipefd[0]); close(pipefd[1]);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        FAIL("fork() failed");
        close(pipefd[0]); close(pipefd[1]);
        close(result[0]); close(result[1]);
        return;
    }

    if (pid == 0) {
        /* Child: try writing to the CLOEXEC pipe — should succeed
         * because POSIX says CLOEXEC only takes effect at exec */
        close(result[0]);
        char c;
        if (write(pipefd[1], "x", 1) == 1)
            c = 'Y'; /* FD was inherited (POSIX correct) */
        else
            c = 'N'; /* FD not available (POSIX violation) */
        write(result[1], &c, 1);
        close(result[1]);
        _exit(0);
    }

    /* Parent: close write ends before reading to avoid deadlock */
    close(result[1]);
    close(pipefd[0]);
    close(pipefd[1]);

    char status_char = 0;
    read(result[0], &status_char, 1);
    close(result[0]);

    int wstatus;
    waitpid(pid, &wstatus, 0);

    if (status_char == 'Y')
        PASS();
    else
        FAIL("CLOEXEC FD not inherited in fork child");
}

/* ---------- Main ---------- */

int main(void) {
    printf("test-cloexec: O_CLOEXEC behavior tests\n");

    test_pipe2_cloexec();
    test_pipe_no_cloexec();
    test_fcntl_setfd();
    test_dup_clears_cloexec();
    test_dup3_cloexec();
    test_no_cloexec_survives_fork();
    test_cloexec_inherited_in_fork();

    SUMMARY("test-cloexec");
    return fails > 0 ? 1 : 0;
}
