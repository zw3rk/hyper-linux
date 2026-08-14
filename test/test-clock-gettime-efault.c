/* test-clock-gettime-efault.c — clock_gettime EFAULT error-path test
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keeps the invalid-pointer clock_gettime syscall isolated because direct
 * Rosetta for Linux raises SIGSEGV instead of returning EFAULT for this case.
 */
#include "test-harness.h"
#include "raw-syscall.h"
#include <stdint.h>
#include <sys/syscall.h>

int passes = 0, fails = 0;

int main(void) {
    void *bad_ptr = (void *)0xDEAD000000000000ULL;

    printf("test-clock-gettime-efault: clock_gettime error path\n");

    TEST("clock_gettime(bad_ptr) → EFAULT");
    fflush(stdout);
    {
        long r = raw_syscall2(__NR_clock_gettime, 0 /* CLOCK_REALTIME */,
                              (long)bad_ptr);
        if (r == -14) PASS();
        else FAIL("expected -EFAULT (-14)");
    }

    SUMMARY("test-clock-gettime-efault");
    return fails > 0 ? 1 : 0;
}
