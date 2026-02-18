/* hello-write.c — direct write(2) hello world
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: sys_write (nr 64) with fd=stdout. Bypasses musl stdio buffering. */

#include <unistd.h>
#include <string.h>

int main(void) {
    const char *msg = "Hello from write!\n";
    write(STDOUT_FILENO, msg, strlen(msg));
    return 0;
}
