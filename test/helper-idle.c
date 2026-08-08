/* helper-idle.c — burn CPU without issuing a single syscall.
 *
 * Used by test-diagnostics.sh to check that a SIGUSR1 stats dump still
 * happens when the guest issues no syscalls at all: the service point on
 * the syscall path never runs, so the latched request was simply lost.
 *
 * A blocking poll() would NOT do — hl returns EINTR from an indefinite
 * wait when it is nudged, the guest loops, and that retry is itself a
 * syscall that services the request. Pure computation is the real case.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("idle\n");
    /* Bounded so a failing run cannot wedge the suite. */
    volatile uint64_t x = 0;
    for (uint64_t i = 0; i < 40000000000ULL; i++)
        x += i;
    return (int)(x & 1);
}
