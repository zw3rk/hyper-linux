/* hello-musl.c — musl printf/puts hello world
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: stdio buffered output via musl puts → fputs → __fwritex → writev.
 * This is the test that exposed the X5 vector clobbering bug. */

#include <stdio.h>

int main(void) {
    printf("Hello from musl!\n");
    return 0;
}
