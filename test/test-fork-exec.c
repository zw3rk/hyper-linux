/* test-fork-exec.c — Fork edge case tests
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests fork edge cases: fork + exec chain, fork with many FDs,
 * fork + pipe communication, and nested fork.
 *
 * Usage: hl test-fork-exec <path-to-echo-test>
 *
 * Syscalls exercised: clone/fork, execve(221), wait4(260), pipe2(59),
 *                     read(63), write(64), close(57), dup2/dup3(24)
 */
#include "test-harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

int passes = 0, fails = 0;

/* Path to echo-test binary (passed via argv) */
static const char *echo_test_path = NULL;

/* ---------- Test 1: Fork + exec chain ---------- */

static void test_fork_exec(void) {
    TEST("fork + exec echo-test");

    if (!echo_test_path) {
        FAIL("no echo-test path provided");
        return;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        FAIL("pipe() failed");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        FAIL("fork() failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        /* Child: redirect stdout to pipe write end, exec echo-test */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        extern char **environ;
        char *argv[] = { (char *)echo_test_path, "fork-exec-ok", NULL };
        execve(echo_test_path, argv, environ);
        _exit(127); /* execve failed */
    }

    /* Parent: read from pipe */
    close(pipefd[1]);
    char buf[256];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);

    if (n > 0) {
        buf[n] = '\0';
        if (strstr(buf, "fork-exec-ok") != NULL)
            PASS();
        else
            FAIL("exec output missing expected string");
    } else {
        FAIL("no output from exec'd child");
    }
}

/* ---------- Test 2: Fork with pipe communication ---------- */

static void test_fork_pipe(void) {
    TEST("fork + pipe communication");

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        FAIL("pipe() failed");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        FAIL("fork() failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        /* Child: write to pipe and exit */
        close(pipefd[0]);
        const char *msg = "hello from child";
        write(pipefd[1], msg, strlen(msg));
        close(pipefd[1]);
        _exit(0);
    }

    /* Parent: read from pipe */
    close(pipefd[1]);
    char buf[64];
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);

    if (n > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        buf[n] = '\0';
        if (strcmp(buf, "hello from child") == 0)
            PASS();
        else
            FAIL("wrong message from child");
    } else {
        FAIL("child communication failed");
    }
}

/* ---------- Test 3: Fork preserves open FDs ---------- */

static void test_fork_fd_inherit(void) {
    TEST("fork preserves open FDs");

    /* Open several pipes, fork, have child verify they're accessible */
    int pipes[4][2];
    for (int i = 0; i < 4; i++) {
        if (pipe(pipes[i]) != 0) {
            FAIL("pipe() failed");
            return;
        }
    }

    /* Create a result pipe for child to report back */
    int result_pipe[2];
    if (pipe(result_pipe) != 0) {
        FAIL("pipe() failed");
        for (int i = 0; i < 4; i++) { close(pipes[i][0]); close(pipes[i][1]); }
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        FAIL("fork() failed");
        return;
    }

    if (pid == 0) {
        /* Child: try writing to each pipe write end */
        close(result_pipe[0]);
        int ok = 1;
        for (int i = 0; i < 4; i++) {
            close(pipes[i][0]); /* Close read ends */
            if (write(pipes[i][1], "x", 1) != 1) ok = 0;
            close(pipes[i][1]);
        }
        char result = ok ? 'Y' : 'N';
        write(result_pipe[1], &result, 1);
        close(result_pipe[1]);
        _exit(0);
    }

    /* Parent: close write ends, read from result pipe */
    close(result_pipe[1]);
    for (int i = 0; i < 4; i++) close(pipes[i][1]);

    char result = 0;
    read(result_pipe[0], &result, 1);
    close(result_pipe[0]);

    /* Also read from pipes to verify child wrote */
    int all_read = 1;
    for (int i = 0; i < 4; i++) {
        char c;
        if (read(pipes[i][0], &c, 1) != 1 || c != 'x') all_read = 0;
        close(pipes[i][0]);
    }

    int status;
    waitpid(pid, &status, 0);

    if (result == 'Y' && all_read && WIFEXITED(status))
        PASS();
    else
        FAIL("child could not access inherited FDs");
}

/* ---------- Test 4: Fork child exit code ---------- */

static void test_fork_exit_codes(void) {
    TEST("fork child exit codes");

    int codes[] = { 0, 1, 42, 127, 255 };
    int ok = 1;

    for (int i = 0; i < 5; i++) {
        pid_t pid = fork();
        if (pid < 0) { ok = 0; break; }
        if (pid == 0) _exit(codes[i]);

        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != codes[i]) {
            ok = 0;
            break;
        }
    }

    if (ok) PASS();
    else FAIL("exit code not preserved");
}

/* ---------- Test 5: Nested fork (parent → child → grandchild) ---------- */

static void test_nested_fork(void) {
    TEST("nested fork (grandchild)");

    int result_pipe[2];
    if (pipe(result_pipe) != 0) {
        FAIL("pipe() failed");
        return;
    }

    pid_t child = fork();
    if (child < 0) {
        FAIL("fork() failed");
        close(result_pipe[0]);
        close(result_pipe[1]);
        return;
    }

    if (child == 0) {
        /* Child: fork again to create grandchild */
        close(result_pipe[0]);

        pid_t grandchild = fork();
        if (grandchild < 0) {
            char msg = 'E'; /* Error */
            write(result_pipe[1], &msg, 1);
            close(result_pipe[1]);
            _exit(1);
        }

        if (grandchild == 0) {
            /* Grandchild: report success via pipe */
            char msg = 'G'; /* Grandchild ran */
            write(result_pipe[1], &msg, 1);
            close(result_pipe[1]);
            _exit(0);
        }

        /* Child: wait for grandchild then exit */
        int status;
        waitpid(grandchild, &status, 0);
        close(result_pipe[1]);
        _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
    }

    /* Parent: read result from pipe */
    close(result_pipe[1]);
    char msg = 0;
    read(result_pipe[0], &msg, 1);
    close(result_pipe[0]);

    int status;
    waitpid(child, &status, 0);

    if (msg == 'G' && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        PASS();
    else
        FAIL("grandchild did not run or exited abnormally");
}

/* ---------- Main ---------- */

int main(int argc, char **argv) {
    printf("test-fork-exec: fork edge case tests\n");

    if (argc >= 2) {
        echo_test_path = argv[1];
    }

    test_fork_exec();
    test_fork_pipe();
    test_fork_fd_inherit();
    test_fork_exit_codes();
    test_nested_fork();

    SUMMARY("test-fork-exec");
    return fails > 0 ? 1 : 0;
}
