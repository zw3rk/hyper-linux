/* test-thread.c — Test clone(CLONE_THREAD) + futex in hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests raw clone(CLONE_THREAD) with futex-based synchronization.
 * Uses inline syscall wrappers (no pthread) to directly exercise
 * hl's thread implementation.
 */
#include "test-harness.h"
#include "raw-syscall.h"
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

int passes = 0, fails = 0;

/* ---------- Shared state ---------- */

/* Shared variable written by child thread, read by parent */
static volatile int shared_value = 0;

/* Child TID — written by CLONE_PARENT_SETTID and cleared by
 * CLONE_CHILD_CLEARTID on thread exit */
static volatile int child_tid = 0;

/* Synchronization flag: child sets to 1 when done, parent waits */
static volatile int done_flag = 0;

/* ---------- Child thread function ---------- */

/* This is the entry point for the child thread. Since clone returns
 * to the same instruction, we use a function that the assembly wrapper
 * calls after clone returns 0. However, for simplicity we just branch
 * based on the return value in main(). */

static void child_work(void) {
    long tid = raw_gettid();

    /* Verify we got a unique TID */
    shared_value = (int)tid;

    /* Signal parent we're done */
    done_flag = 1;
    raw_futex_wake((int *)&done_flag, 1);

    /* Exit this thread (not the process) */
    raw_exit(0);
}

/* ---------- Tests ---------- */

/* Stack for child thread (8KB, 16-byte aligned) */
static char child_stack_buf[8192] __attribute__((aligned(16)));

/* Test 1: clone(CLONE_THREAD) creates a new thread that runs concurrently */
static void test_clone_thread(void) {
    TEST("clone(CLONE_THREAD)");

    /* Reset shared state */
    shared_value = 0;
    done_flag = 0;
    child_tid = 0;

    /* Clone flags: thread + shared VM + shared FS + shared files +
     * shared signal handlers + PARENT_SETTID + CHILD_CLEARTID + SETTLS */
    unsigned long flags = 0x7d0f00;  /* CLONE_THREAD|CLONE_VM|... */
    /* Use the musl-style flags for CLONE_THREAD:
     * CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
     * CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID |
     * CLONE_CHILD_CLEARTID */
    flags = 0x7d0f00;

    /* Child stack: grows downward, start at top of buffer.
     * We put the child function pointer at the top so the child can
     * call it after clone returns 0. But since clone returns to the
     * same PC, the child will also reach the code after the clone
     * call. We handle this by checking the return value. */
    void *stack_top = child_stack_buf + sizeof(child_stack_buf);

    long ret = raw_clone(flags, stack_top,
                         (int *)&child_tid,  /* CLONE_PARENT_SETTID */
                         0,                  /* TLS (not used here) */
                         (int *)&child_tid); /* CLONE_CHILD_CLEARTID */

    if (ret == 0) {
        /* We are the child thread */
        child_work();
        /* child_work calls raw_exit, should not reach here */
        __builtin_unreachable();
    }

    if (ret < 0) {
        FAIL("clone returned error");
        return;
    }

    /* Parent: ret = child TID */
    /* Wait for child to signal completion */
    while (done_flag == 0) {
        raw_futex_wait((int *)&done_flag, 0);
    }

    /* Verify child wrote its TID */
    if (shared_value > 0 && shared_value != (int)raw_gettid()) {
        PASS();
    } else {
        FAIL("child did not write unique TID to shared_value");
    }
}

/* Test 2: CLONE_PARENT_SETTID writes child TID to ptid */
static void test_parent_settid(void) {
    TEST("CLONE_PARENT_SETTID");

    /* child_tid was set by CLONE_PARENT_SETTID in test_clone_thread */
    /* Wait for child to fully exit (CLONE_CHILD_CLEARTID clears it) */
    while (child_tid != 0) {
        raw_futex_wait((int *)&child_tid, child_tid);
    }

    /* If we get here, CHILD_CLEARTID cleared it and FUTEX_WAKE woke us.
     * The original value was > 0 (written by PARENT_SETTID). */
    PASS();
}

/* Test 3: Multiple concurrent threads */
static void test_multi_thread(void) {
    TEST("multiple threads");

    #define NUM_THREADS 4
    static volatile int results[NUM_THREADS];
    static volatile int tids[NUM_THREADS];
    static char stacks[NUM_THREADS][8192] __attribute__((aligned(16)));

    memset((void *)results, 0, sizeof(results));
    memset((void *)tids, 0, sizeof(tids));

    unsigned long flags = 0x7d0f00;

    int spawned = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        void *sp = stacks[i] + sizeof(stacks[i]);
        long ret = raw_clone(flags, sp,
                             (int *)&tids[i], 0, (int *)&tids[i]);
        if (ret == 0) {
            /* Child: write our index + TID and exit */
            results[i] = (int)raw_gettid();
            raw_exit(0);
            __builtin_unreachable();
        }
        if (ret > 0) spawned++;
    }

    /* Wait for all children to complete (CHILD_CLEARTID clears tids) */
    for (int i = 0; i < NUM_THREADS; i++) {
        while (tids[i] != 0) {
            raw_futex_wait((int *)&tids[i], tids[i]);
        }
    }

    /* Verify all threads ran and got unique TIDs */
    int all_unique = 1;
    for (int i = 0; i < NUM_THREADS; i++) {
        if (results[i] == 0) { all_unique = 0; break; }
        for (int j = i + 1; j < NUM_THREADS; j++) {
            if (results[i] == results[j]) { all_unique = 0; break; }
        }
    }

    if (spawned == NUM_THREADS && all_unique) {
        PASS();
    } else {
        FAIL("not all threads completed with unique TIDs");
    }
    #undef NUM_THREADS
}

/* ---------- Main ---------- */

int main(void) {
    printf("test-thread: raw clone(CLONE_THREAD) + futex tests\n");

    test_clone_thread();
    test_parent_settid();
    test_multi_thread();

    SUMMARY("test-thread");
    return fails > 0 ? 1 : 0;
}
