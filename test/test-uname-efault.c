/* test-uname-efault.c — uname EFAULT error-path test
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keeps the invalid-pointer uname syscall isolated so execution environments
 * can report its EFAULT contract independently.
 */
#include "test-harness.h"
#include "raw-syscall.h"
#include <stdint.h>
#include <sys/syscall.h>

int passes = 0, fails = 0;

int main(void) {
    void *bad_ptr = (void *)0xDEAD000000000000ULL;

    printf("test-uname-efault: uname error path\n");

    TEST("uname(bad_ptr) → EFAULT");
    fflush(stdout);
    {
        long r = raw_syscall1(__NR_uname, (long)bad_ptr);
        if (r == -14) PASS();
        else FAIL("expected -EFAULT (-14)");
    }

    SUMMARY("test-uname-efault");
    return fails > 0 ? 1 : 0;
}
