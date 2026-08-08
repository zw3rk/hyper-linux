/* test-vdso-time.c — Verify vDSO gettimeofday/clock_gettime without SVC storm
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Calls gettimeofday / clock_gettime many times. Host HL_SYSCALL_STATS
 * should show ~0 SYS_gettimeofday / SYS_clock_gettime if vDSO works.
 * Also checks monotonicity and basic coherence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define N 50000

static long long ts_ns(const struct timespec *t) {
    return (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
}

int main(void) {
    struct timeval tv0, tv1;
    struct timespec mono0, mono1, rt0, rt1;

    if (gettimeofday(&tv0, NULL) != 0) {
        perror("gettimeofday");
        return 1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &mono0) != 0) {
        perror("clock_gettime mono");
        return 1;
    }
    if (clock_gettime(CLOCK_REALTIME, &rt0) != 0) {
        perror("clock_gettime rt");
        return 1;
    }

    /* Hot loops — should hit vDSO, not SVC */
    for (int i = 0; i < N; i++) {
        if (gettimeofday(&tv1, NULL) != 0)
            return 2;
        if (clock_gettime(CLOCK_MONOTONIC, &mono1) != 0)
            return 3;
        if (clock_gettime(CLOCK_REALTIME, &rt1) != 0)
            return 4;
        /* Monotonic non-decreasing */
        if (ts_ns(&mono1) < ts_ns(&mono0)) {
            fprintf(stderr, "mono went backwards\n");
            return 5;
        }
        mono0 = mono1;
    }

    /* Coarse coherence: wall clock advanced or stayed within reason */
    long long us0 = (long long)tv0.tv_sec * 1000000LL + tv0.tv_usec;
    long long us1 = (long long)tv1.tv_sec * 1000000LL + tv1.tv_usec;
    if (us1 < us0 - 2000) { /* allow 2ms jitter / step */
        fprintf(stderr, "realtime went backwards a lot: %lld -> %lld\n", us0, us1);
        return 6;
    }

    /* PROCESS_CPUTIME must still work (SVC path) */
    struct timespec cpu;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) != 0) {
        perror("clock_gettime cputime");
        return 7;
    }

    printf("vdso-time ok loops=%d last_mono=%ld.%09ld cputime_ok\n",
           N, (long)mono1.tv_sec, mono1.tv_nsec);
    return 0;
}
