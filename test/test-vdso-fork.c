/* test-vdso-fork.c — the clock must keep advancing in a fork child
 *
 * Regression: the [vvar] time page is written only by the vDSO publisher
 * thread. fork_child_main() never started one, and the COW child maps the
 * page MAP_PRIVATE so it cannot observe the parent's updates either. The
 * child's clock_gettime()/gettimeofday() therefore returned the fork
 * instant forever — and because the inherited page still carried a valid
 * version stamp, the guest took the vDSO fast path instead of falling back
 * to the syscall. Elapsed-time loops in forked children never terminated.
 *
 * v0.2.4 was unaffected: its vDSO was plain SVC trampolines.
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "test-harness.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>

/* Busy-ish wait for at least ms milliseconds, but never more than `cap`
 * iterations so a frozen clock fails the test instead of hanging it. */
static int clock_advances(clockid_t clk, long need_ns, long cap_iters) {
    struct timespec t0, t1;
    if (clock_gettime(clk, &t0) != 0) return -1;
    for (long i = 0; i < cap_iters; i++) {
        if (clock_gettime(clk, &t1) != 0) return -1;
        long long d = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
                      (t1.tv_nsec - t0.tv_nsec);
        if (d >= need_ns) return 1;
        struct timespec nap = { .tv_sec = 0, .tv_nsec = 1000000L };
        nanosleep(&nap, NULL);
    }
    return 0;
}

int main(void) {
    int passes = 0, fails = 0;
    printf("test-vdso-fork: clock must advance in a fork child\n");

    TEST("parent CLOCK_MONOTONIC advances");
    {
        int r = clock_advances(CLOCK_MONOTONIC, 20 * 1000000L, 2000);
        if (r == 1) PASS();
        else FAILF("parent clock did not advance (r=%d)", r);
    }

    TEST("child CLOCK_MONOTONIC advances after fork");
    {
        pid_t pid = fork();
        if (pid < 0) FAILF("fork: %s", strerror(errno));
        else if (pid == 0) {
            int r = clock_advances(CLOCK_MONOTONIC, 20 * 1000000L, 2000);
            _exit(r == 1 ? 0 : 1);
        } else {
            int st = 0;
            if (waitpid(pid, &st, 0) < 0) FAILF("waitpid: %s", strerror(errno));
            else if (WIFEXITED(st) && WEXITSTATUS(st) == 0) PASS();
            else FAILF("child exit status %d (frozen clock?)", st);
        }
    }

    TEST("child CLOCK_REALTIME advances after fork");
    {
        pid_t pid = fork();
        if (pid < 0) FAILF("fork: %s", strerror(errno));
        else if (pid == 0) {
            int r = clock_advances(CLOCK_REALTIME, 20 * 1000000L, 2000);
            _exit(r == 1 ? 0 : 1);
        } else {
            int st = 0;
            if (waitpid(pid, &st, 0) < 0) FAILF("waitpid: %s", strerror(errno));
            else if (WIFEXITED(st) && WEXITSTATUS(st) == 0) PASS();
            else FAILF("child exit status %d (frozen clock?)", st);
        }
    }

    SUMMARY("test-vdso-fork");
    return fails ? 1 : 0;
}
