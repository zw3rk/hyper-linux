/* test-fork.c — Test fork()/waitpid() via clone syscall
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests basic fork semantics: parent forks child, child writes a message
 * and exits with a specific code, parent waits and verifies exit status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    printf("test-fork: forking...\n");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* Child process */
        printf("test-fork: child (pid=%d, ppid=%d)\n", getpid(), getppid());
        _exit(42);
    }

    /* Parent process */
    printf("test-fork: parent (pid=%d), child=%d\n", getpid(), pid);

    int status;
    pid_t waited = waitpid(pid, &status, 0);
    if (waited < 0) {
        perror("waitpid");
        return 1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 42) {
        printf("test-fork: child exited with code 42 — PASS\n");
        return 0;
    }

    printf("test-fork: unexpected status 0x%x — FAIL\n", status);
    return 1;
}
