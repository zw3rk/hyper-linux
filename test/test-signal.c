/* test-signal.c — Test signal delivery and rt_sigreturn
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies:
 * 1. rt_sigaction installs a handler for SIGUSR1
 * 2. kill(getpid(), SIGUSR1) delivers the signal
 * 3. Handler fires with correct signum
 * 4. rt_sigreturn restores all callee-saved registers
 *    (aarch64: X19-X28, x86_64: rbx/r12-r15/rbp)
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
 * The inline asm loads known values into callee-saved registers,
 * sends SIGUSR1 (which triggers delivery + rt_sigreturn), then
 * verifies the registers are unchanged.
 *
 * aarch64 callee-saved: X19-X28 (10 registers)
 * x86_64 callee-saved:  rbx, r12-r15 (5 registers; rbp excluded
 *   because GCC needs it as frame pointer in functions with calls)
 */
static int test_callee_saved(void) {
#if defined(__aarch64__)
    register long r0 __asm__("x19") = 0xDEAD0019;
    register long r1 __asm__("x20") = 0xDEAD0020;
    register long r2 __asm__("x21") = 0xDEAD0021;
    register long r3 __asm__("x22") = 0xDEAD0022;
    register long r4 __asm__("x23") = 0xDEAD0023;
    register long r5 __asm__("x24") = 0xDEAD0024;
    register long r6 __asm__("x25") = 0xDEAD0025;
    register long r7 __asm__("x26") = 0xDEAD0026;
    register long r8 __asm__("x27") = 0xDEAD0027;
    register long r9 __asm__("x28") = 0xDEAD0028;
    #define NUM_CALLEE_SAVED 10
    long expect[] = { 0xDEAD0019, 0xDEAD0020, 0xDEAD0021, 0xDEAD0022,
                      0xDEAD0023, 0xDEAD0024, 0xDEAD0025, 0xDEAD0026,
                      0xDEAD0027, 0xDEAD0028 };
    const char *names[] = { "X19","X20","X21","X22","X23",
                            "X24","X25","X26","X27","X28" };

    __asm__ volatile("" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3),
                          "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7),
                          "+r"(r8), "+r"(r9));

    kill(getpid(), SIGUSR1);

    __asm__ volatile("" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3),
                          "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7),
                          "+r"(r8), "+r"(r9));

    long actual[] = { r0, r1, r2, r3, r4, r5, r6, r7, r8, r9 };

#elif defined(__x86_64__)
    register long r0 __asm__("rbx") = 0xDEAD0001;
    register long r1 __asm__("r12") = 0xDEAD0002;
    register long r2 __asm__("r13") = 0xDEAD0003;
    register long r3 __asm__("r14") = 0xDEAD0004;
    register long r4 __asm__("r15") = 0xDEAD0005;
    #define NUM_CALLEE_SAVED 5
    long expect[] = { 0xDEAD0001, 0xDEAD0002, 0xDEAD0003,
                      0xDEAD0004, 0xDEAD0005 };
    const char *names[] = { "rbx","r12","r13","r14","r15" };

    __asm__ volatile("" : "+r"(r0), "+r"(r1), "+r"(r2),
                          "+r"(r3), "+r"(r4));

    kill(getpid(), SIGUSR1);

    __asm__ volatile("" : "+r"(r0), "+r"(r1), "+r"(r2),
                          "+r"(r3), "+r"(r4));

    long actual[] = { r0, r1, r2, r3, r4 };

#else
#error "Unsupported architecture"
#endif

    int ok = 1;
    for (int i = 0; i < NUM_CALLEE_SAVED; i++) {
        if (actual[i] != expect[i]) {
            printf("  %s corrupted: 0x%lx\n", names[i], actual[i]);
            ok = 0;
        }
    }
    #undef NUM_CALLEE_SAVED
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
