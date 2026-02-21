/* test-pthread.c — Test musl pthread_create/pthread_join in hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Uses the standard POSIX thread API (musl implementation) to verify
 * that hl's clone(CLONE_THREAD) + futex + TLS support is sufficient
 * for real-world threading.
 */
#include "test-harness.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>

int passes = 0, fails = 0;

/* ---------- Test 1: basic thread create + join ---------- */

static void *basic_thread_fn(void *arg) {
    int *result = (int *)arg;
    *result = 42;
    return NULL;
}

static void test_basic_create_join(void) {
    TEST("pthread_create + join");

    int result = 0;
    pthread_t t;
    int err = pthread_create(&t, NULL, basic_thread_fn, &result);
    if (err != 0) {
        FAIL("pthread_create failed");
        return;
    }

    err = pthread_join(t, NULL);
    if (err != 0) {
        FAIL("pthread_join failed");
        return;
    }

    if (result == 42) {
        PASS();
    } else {
        FAIL("thread did not write expected value");
    }
}

/* ---------- Test 2: thread return value ---------- */

static void *return_value_fn(void *arg) {
    (void)arg;
    return (void *)(long)99;
}

static void test_return_value(void) {
    TEST("thread return value");

    pthread_t t;
    int err = pthread_create(&t, NULL, return_value_fn, NULL);
    if (err != 0) {
        FAIL("pthread_create failed");
        return;
    }

    void *retval = NULL;
    err = pthread_join(t, &retval);
    if (err != 0) {
        FAIL("pthread_join failed");
        return;
    }

    if ((long)retval == 99) {
        PASS();
    } else {
        FAIL("wrong return value from thread");
    }
}

/* ---------- Test 3: multiple threads ---------- */

#define NUM_THREADS 4

static void *multi_thread_fn(void *arg) {
    int idx = (int)(long)arg;
    /* Do some work proportional to index (ensures threads overlap) */
    volatile int sum = 0;
    for (int i = 0; i < (idx + 1) * 100; i++)
        sum += i;
    return (void *)(long)sum;
}

static void test_multiple_threads(void) {
    TEST("multiple threads");

    pthread_t threads[NUM_THREADS];
    int err;

    /* Create all threads */
    for (int i = 0; i < NUM_THREADS; i++) {
        err = pthread_create(&threads[i], NULL, multi_thread_fn,
                             (void *)(long)i);
        if (err != 0) {
            FAIL("pthread_create failed");
            return;
        }
    }

    /* Join all and verify return values */
    int all_ok = 1;
    for (int i = 0; i < NUM_THREADS; i++) {
        void *retval;
        err = pthread_join(threads[i], &retval);
        if (err != 0) {
            all_ok = 0;
            break;
        }
        /* Verify expected sum: sum(0..n-1) where n=(i+1)*100 */
        int n = (i + 1) * 100;
        long expected = (long)n * (n - 1) / 2;
        if ((long)retval != expected) {
            all_ok = 0;
            break;
        }
    }

    if (all_ok) {
        PASS();
    } else {
        FAIL("some threads returned wrong values");
    }
}

/* ---------- Test 4: shared memory between threads ---------- */

static volatile int shared_counter = 0;

static void *increment_fn(void *arg) {
    int count = (int)(long)arg;
    for (int i = 0; i < count; i++) {
        /* Non-atomic increment — we only care that the threads
         * actually share memory, not about race conditions */
        shared_counter++;
    }
    return NULL;
}

static void test_shared_memory(void) {
    TEST("shared memory");

    shared_counter = 0;

    /* Use a single thread to avoid race condition complexity.
     * This tests that the thread can read/write the parent's memory. */
    pthread_t t;
    int err = pthread_create(&t, NULL, increment_fn, (void *)(long)1000);
    if (err != 0) {
        FAIL("pthread_create failed");
        return;
    }

    pthread_join(t, NULL);

    if (shared_counter == 1000) {
        PASS();
    } else {
        FAIL("shared counter not updated correctly");
    }
}

/* ---------- Test 5: thread-local storage (TLS) ---------- */

static __thread int tls_value = 0;

static void *tls_thread_fn(void *arg) {
    int my_val = (int)(long)arg;
    tls_value = my_val;
    /* Yield to let other threads run */
    for (volatile int i = 0; i < 10000; i++) {}
    /* Verify TLS is still our value (not clobbered by other threads) */
    if (tls_value == my_val)
        return (void *)(long)1;  /* success */
    return (void *)(long)0;      /* failure */
}

static void test_tls(void) {
    TEST("thread-local storage");

    #define TLS_THREADS 3
    pthread_t threads[TLS_THREADS];

    for (int i = 0; i < TLS_THREADS; i++) {
        int err = pthread_create(&threads[i], NULL, tls_thread_fn,
                                 (void *)(long)(i + 100));
        if (err != 0) {
            FAIL("pthread_create failed");
            return;
        }
    }

    int all_ok = 1;
    for (int i = 0; i < TLS_THREADS; i++) {
        void *retval;
        pthread_join(threads[i], &retval);
        if ((long)retval != 1) all_ok = 0;
    }

    if (all_ok) {
        PASS();
    } else {
        FAIL("TLS values were clobbered between threads");
    }
    #undef TLS_THREADS
}

/* ---------- Main ---------- */

int main(void) {
    printf("test-pthread: musl pthread API tests\n");

    test_basic_create_join();
    test_return_value();
    test_multiple_threads();
    test_shared_memory();
    test_tls();

    printf("\ntest-pthread: %d passed, %d failed\n", passes, fails);
    return fails > 0 ? 1 : 0;
}
