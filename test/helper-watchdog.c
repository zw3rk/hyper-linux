/* helper-watchdog.c — loop for N seconds making NO VM exits.
 *
 * Times itself with vDSO gettimeofday (reads the [vvar] page; no syscall,
 * no VM exit), so it both triggers hl's per-iteration watchdog and knows
 * when to stop. Used by test-diagnostics.sh:
 *   - by default (watchdog off) it runs to completion and prints "wd-done";
 *   - with --timeout T (T < N) hl kills it before it finishes.
 *
 * argv[1] = seconds to run (default 12, chosen to exceed the old 10s default
 * so a regression to that default is caught).
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    long secs = (argc > 1) ? atol(argv[1]) : 12;
    struct timeval t0, t;
    gettimeofday(&t0, NULL);
    printf("wd-start %ld\n", secs);
    for (;;) {
        gettimeofday(&t, NULL);          /* vDSO: no VM exit */
        if (t.tv_sec - t0.tv_sec >= secs) break;
    }
    printf("wd-done\n");
    return 0;
}
