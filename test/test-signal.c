/* test-signal.c — Test signal delivery and rt_sigreturn
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies:
 * 1. rt_sigaction installs a handler for SIGUSR1
 * 2. kill(getpid(), SIGUSR1) delivers the signal
 * 3. Handler fires with correct signum
 * 4. rt_sigreturn restores all callee-saved registers (X19-X28)
 * 5. sigprocmask blocks/unblocks signals correctly
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static volatile sig_atomic_t handler_called = 0;
static volatile int handler_signum = 0;

static void sigusr1_handler(int sig) {
    handler_called = 1;
    handler_signum = sig;
}

/* Test that callee-saved registers survive signal delivery.
 * The inline asm loads known values into X19-X28, sends SIGUSR1
 * (which triggers delivery + rt_sigreturn), then verifies the
 * registers are unchanged. */
static int test_callee_saved(void) {
    register long x19 __asm__("x19") = 0xDEAD0019;
    register long x20 __asm__("x20") = 0xDEAD0020;
    register long x21 __asm__("x21") = 0xDEAD0021;
    register long x22 __asm__("x22") = 0xDEAD0022;
    register long x23 __asm__("x23") = 0xDEAD0023;
    register long x24 __asm__("x24") = 0xDEAD0024;
    register long x25 __asm__("x25") = 0xDEAD0025;
    register long x26 __asm__("x26") = 0xDEAD0026;
    register long x27 __asm__("x27") = 0xDEAD0027;
    register long x28 __asm__("x28") = 0xDEAD0028;

    /* Force compiler to assign the register variables */
    __asm__ volatile("" : "+r"(x19), "+r"(x20), "+r"(x21), "+r"(x22),
                          "+r"(x23), "+r"(x24), "+r"(x25), "+r"(x26),
                          "+r"(x27), "+r"(x28));

    /* Send signal — handler will fire, rt_sigreturn restores regs */
    kill(getpid(), SIGUSR1);

    /* Prevent compiler from reloading register values from stack */
    __asm__ volatile("" : "+r"(x19), "+r"(x20), "+r"(x21), "+r"(x22),
                          "+r"(x23), "+r"(x24), "+r"(x25), "+r"(x26),
                          "+r"(x27), "+r"(x28));

    int ok = 1;
    if (x19 != 0xDEAD0019) { printf("  X19 corrupted: 0x%lx\n", x19); ok = 0; }
    if (x20 != 0xDEAD0020) { printf("  X20 corrupted: 0x%lx\n", x20); ok = 0; }
    if (x21 != 0xDEAD0021) { printf("  X21 corrupted: 0x%lx\n", x21); ok = 0; }
    if (x22 != 0xDEAD0022) { printf("  X22 corrupted: 0x%lx\n", x22); ok = 0; }
    if (x23 != 0xDEAD0023) { printf("  X23 corrupted: 0x%lx\n", x23); ok = 0; }
    if (x24 != 0xDEAD0024) { printf("  X24 corrupted: 0x%lx\n", x24); ok = 0; }
    if (x25 != 0xDEAD0025) { printf("  X25 corrupted: 0x%lx\n", x25); ok = 0; }
    if (x26 != 0xDEAD0026) { printf("  X26 corrupted: 0x%lx\n", x26); ok = 0; }
    if (x27 != 0xDEAD0027) { printf("  X27 corrupted: 0x%lx\n", x27); ok = 0; }
    if (x28 != 0xDEAD0028) { printf("  X28 corrupted: 0x%lx\n", x28); ok = 0; }
    return ok;
}

int main(void) {
    int failures = 0;

    /* Test 1: Basic signal delivery */
    printf("test-signal: 1. basic signal delivery... ");
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        printf("FAIL (sigaction: %m)\n");
        return 1;
    }
    kill(getpid(), SIGUSR1);
    if (handler_called && handler_signum == SIGUSR1) {
        printf("PASS\n");
    } else {
        printf("FAIL (called=%d, signum=%d)\n", handler_called, handler_signum);
        failures++;
    }

    /* Test 2: Callee-saved register preservation */
    printf("test-signal: 2. callee-saved register preservation... ");
    handler_called = 0;
    if (test_callee_saved() && handler_called) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
        failures++;
    }

    /* Test 3: Signal blocking with sigprocmask */
    printf("test-signal: 3. signal blocking (sigprocmask)... ");
    handler_called = 0;
    sigset_t block_set, old_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block_set, &old_set);

    kill(getpid(), SIGUSR1);  /* Should be queued, not delivered */
    if (handler_called) {
        printf("FAIL (signal delivered while blocked)\n");
        failures++;
    } else {
        /* Unblock — signal should deliver now */
        sigprocmask(SIG_SETMASK, &old_set, NULL);
        if (handler_called) {
            printf("PASS\n");
        } else {
            printf("FAIL (signal not delivered after unblock)\n");
            failures++;
        }
    }

    /* Test 4: SA_RESETHAND */
    printf("test-signal: 4. SA_RESETHAND... ");
    handler_called = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    kill(getpid(), SIGUSR1);
    if (!handler_called) {
        printf("FAIL (handler not called)\n");
        failures++;
    } else {
        /* Handler should have been reset to SIG_DFL.
         * Sending SIGUSR1 again should terminate the process,
         * but we can't test that directly. Instead, check that
         * the old action is SIG_DFL now. */
        struct sigaction old;
        sigaction(SIGUSR1, NULL, &old);
        if (old.sa_handler == SIG_DFL) {
            printf("PASS\n");
        } else {
            printf("FAIL (handler not reset to SIG_DFL)\n");
            failures++;
        }
    }

    if (failures == 0) {
        printf("test-signal: all tests passed — PASS\n");
        return 0;
    }
    printf("test-signal: %d test(s) failed — FAIL\n", failures);
    return 1;
}
