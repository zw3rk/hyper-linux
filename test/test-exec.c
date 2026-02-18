/* test-exec.c — Test execve() syscall
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Usage: hl test-exec <path-to-echo-test> exec-works
 *
 * This test calls execve() to replace itself with echo-test, which
 * prints its arguments. If execve works correctly, "exec-works" is
 * printed and the process exits 0.
 */
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: test-exec <elf-path> <arg...>\n");
        return 1;
    }

    /* argv[1] = path to binary to exec
     * argv[2..] = arguments to pass to it */
    char *new_argv[64];
    int n = 0;
    for (int i = 1; i < argc && n < 63; i++) {
        new_argv[n++] = argv[i];
    }
    new_argv[n] = NULL;

    /* Collect environment */
    extern char **environ;

    /* Execute the new binary — this should NOT return */
    execve(argv[1], new_argv, environ);

    /* If we get here, execve failed */
    perror("execve");
    return 1;
}
