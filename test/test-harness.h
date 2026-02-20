/* test-harness.h — Shared test macros for hl unit tests
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provides TEST/PASS/FAIL macros used by all test-*.c files.
 * Each test file must declare `int passes = 0, fails = 0;` before using.
 */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <errno.h>

#define TEST(name) printf("  %-30s ", name)
#define PASS()     do { printf("OK\n"); passes++; } while(0)
#define FAIL(msg)  do { printf("FAIL: %s (errno=%d)\n", msg, errno); fails++; } while(0)

#endif /* TEST_HARNESS_H */
