/* test-signal-thread.c — Signal + thread interaction tests
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests signal delivery via tgkill, multiple sequential signals,
 * and signal mask operations. Avoids concurrent thread + signal
 * delivery which requires per-thread signal masks (not yet implemented).
 *
 * Syscalls exercised: tgkill(131), kill(129), rt_sigaction(134),
 *                     rt_sigprocmask(135), getpid(172), gettid(178)
 */
#include "test-harness.h"
#include "raw-syscall.h"
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

int passes = 0, fails = 0;

/* ---------- Signal state ---------- */

static volatile sig_atomic_t sig_received = 0;
static volatile int sig_receiver_tid = 0;

static void sigusr1_handler(int sig) {
    (void)sig;
    sig_received = 1;
    sig_receiver_tid = (int)raw_gettid();
}

/* ---------- Test 1: tgkill sends signal to self ---------- */

static void test_tgkill_self(void) {
    TEST("tgkill(self) delivers signal");

    sig_received = 0;
    sig_receiver_tid = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    int pid = (int)raw_getpid();
    int tid = (int)raw_gettid();
    long ret = raw_tgkill(pid, tid, SIGUSR1);

    if (ret == 0 && sig_received && sig_receiver_tid == tid)
        PASS();
    else
        FAIL("tgkill to self did not deliver");
}

/* ---------- Test 2: Multiple signals in sequence ---------- */

static volatile int multi_sig_count = 0;

static void multi_sig_handler(int sig) {
    (void)sig;
    multi_sig_count++;
}

static void test_multiple_signals(void) {
    TEST("multiple signals in sequence");

    multi_sig_count = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = multi_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    /* Send 5 signals in sequence */
    for (int i = 0; i < 5; i++) {
        kill(getpid(), SIGUSR1);
    }

    if (multi_sig_count >= 5)
        PASS();
    else
        FAIL("not all signals delivered");
}

/* ---------- Test 3: Signal blocked then unblocked ---------- */

static void test_signal_block_unblock(void) {
    TEST("block + unblock delivers pending");

    sig_received = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    /* Block SIGUSR1 */
    sigset_t block_set, old_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block_set, &old_set);

    /* Send signal while blocked — should be queued */
    kill(getpid(), SIGUSR1);

    if (sig_received) {
        FAIL("signal delivered while blocked");
        return;
    }

    /* Unblock — pending signal should deliver now */
    sigprocmask(SIG_SETMASK, &old_set, NULL);

    if (sig_received)
        PASS();
    else
        FAIL("pending signal not delivered after unblock");
}

/* ---------- Test 4: tgkill with wrong TID → ESRCH ---------- */

static void test_tgkill_bad_tid(void) {
    TEST("tgkill(bad tid) → ESRCH");

    int pid = (int)raw_getpid();
    /* Use a TID that doesn't exist */
    long ret = raw_tgkill(pid, 99999, SIGUSR1);
    /* Linux returns -ESRCH = -3 */
    if (ret == -3)
        PASS();
    else
        FAIL("expected -ESRCH");
}

/* ---------- Test 5: SA_RESETHAND resets handler ---------- */

static volatile sig_atomic_t resethand_called = 0;

static void resethand_handler(int sig) {
    (void)sig;
    resethand_called = 1;
}

static void test_sa_resethand(void) {
    TEST("SA_RESETHAND resets to SIG_DFL");

    resethand_called = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = resethand_handler;
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    kill(getpid(), SIGUSR1);

    if (!resethand_called) {
        FAIL("handler not called");
        return;
    }

    /* Verify handler was reset to SIG_DFL */
    struct sigaction old;
    sigaction(SIGUSR1, NULL, &old);
    if (old.sa_handler == SIG_DFL)
        PASS();
    else
        FAIL("handler not reset to SIG_DFL");
}

/* ---------- Main ---------- */

int main(void) {
    printf("test-signal-thread: signal delivery tests\n");

    test_tgkill_self();
    test_multiple_signals();
    test_signal_block_unblock();
    test_tgkill_bad_tid();
    test_sa_resethand();

    printf("\ntest-signal-thread: %d passed, %d failed\n", passes, fails);
    return fails > 0 ? 1 : 0;
}
